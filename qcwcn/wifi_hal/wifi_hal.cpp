/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdint.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/family.h>
#include <netlink/genl/ctrl.h>
#include <linux/rtnetlink.h>
#include <netpacket/packet.h>
#include <linux/filter.h>
#include <linux/errqueue.h>

#include <linux/pkt_sched.h>
#include <netlink/object-api.h>
#include <netlink/netlink.h>
#include <netlink/socket.h>
#include <netlink-types.h>

#include "nl80211_copy.h"

#include <dirent.h>
#include <net/if.h>

#include "sync.h"

#define LOG_TAG  "WifiHAL"

#include "hardware_legacy/wifi.h"

#include "wifi_hal.h"
#include "common.h"
#include "cpp_bindings.h"
#include "ifaceeventhandler.h"

/*
 BUGBUG: normally, libnl allocates ports for all connections it makes; but
 being a static library, it doesn't really know how many other netlink
 connections are made by the same process, if connections come from different
 shared libraries. These port assignments exist to solve that
 problem - temporarily. We need to fix libnl to try and allocate ports across
 the entire process.
 */

#define WIFI_HAL_CMD_SOCK_PORT       644
#define WIFI_HAL_EVENT_SOCK_PORT     645

static void internal_event_handler(wifi_handle handle, int events);
static int internal_valid_message_handler(nl_msg *msg, void *arg);
static int wifi_get_multicast_id(wifi_handle handle, const char *name,
        const char *group);
static int wifi_add_membership(wifi_handle handle, const char *group);
static wifi_error wifi_init_interfaces(wifi_handle handle);

/* Initialize/Cleanup */

wifi_interface_handle wifi_get_iface_handle(wifi_handle handle, char *name)
{
    hal_info *info = (hal_info *)handle;
    for (int i=0;i<info->num_interfaces;i++)
    {
        if (!strcmp(info->interfaces[i]->name, name))
        {
            return ((wifi_interface_handle )(info->interfaces)[i]);
        }
    }
    return NULL;
}

void wifi_socket_set_local_port(struct nl_sock *sock, uint32_t port)
{
    uint32_t pid = getpid() & 0x3FFFFF;

    if (port == 0) {
        sock->s_flags &= ~NL_OWN_PORT;
    } else {
        sock->s_flags |= NL_OWN_PORT;
    }

    sock->s_local.nl_pid = pid + (port << 22);
}

static nl_sock * wifi_create_nl_socket(int port)
{
    // ALOGI("Creating socket");
    struct nl_sock *sock = nl_socket_alloc();
    if (sock == NULL) {
        ALOGE("Could not create handle");
        return NULL;
    }

    wifi_socket_set_local_port(sock, port);

    struct sockaddr_nl *addr_nl = &(sock->s_local);
    /* ALOGI("socket address is %d:%d:%d:%d",
       addr_nl->nl_family, addr_nl->nl_pad, addr_nl->nl_pid,
       addr_nl->nl_groups); */

    struct sockaddr *addr = NULL;
    // ALOGI("sizeof(sockaddr) = %d, sizeof(sockaddr_nl) = %d", sizeof(*addr),
    // sizeof(*addr_nl));

    // ALOGI("Connecting socket");
    if (nl_connect(sock, NETLINK_GENERIC)) {
        ALOGE("Could not connect handle");
        nl_socket_free(sock);
        return NULL;
    }

    ALOGI("Socket Value:%p", sock);
    return sock;
}

int ack_handler(struct nl_msg *msg, void *arg)
{
    int *err = (int *)arg;
    *err = 0;
    ALOGD("%s invoked",__func__);
    return NL_STOP;
}

int finish_handler(struct nl_msg *msg, void *arg)
{
    int *ret = (int *)arg;
    *ret = 0;
    ALOGD("%s called",__func__);
    return NL_SKIP;
}

int error_handler(struct sockaddr_nl *nla,
                  struct nlmsgerr *err, void *arg)
{
    int *ret = (int *)arg;
    *ret = err->error;

    ALOGD("%s invoked with error: %d", __func__, err->error);
    return NL_SKIP;
}
static int no_seq_check(struct nl_msg *msg, void *arg)
{
    ALOGD("no_seq_check received");
    return NL_OK;
}

static wifi_error acquire_supported_features(wifi_interface_handle iface,
        feature_set *set)
{
    int ret = 0;
    interface_info *iinfo = getIfaceInfo(iface);
    wifi_handle handle = getWifiHandle(iface);
    *set = 0;

    WifihalGeneric supportedFeatures(handle, 0,
            OUI_QCA,
            QCA_NL80211_VENDOR_SUBCMD_GET_SUPPORTED_FEATURES);

    /* create the message */
    ret = supportedFeatures.create();
    if (ret < 0)
        goto cleanup;

    ret = supportedFeatures.set_iface_id(iinfo->name);
    if (ret < 0)
        goto cleanup;

    ret = supportedFeatures.requestResponse();
    if (ret != 0) {
        ALOGE("%s: requestResponse Error:%d",__func__, ret);
        goto cleanup;
    }

    supportedFeatures.getResponseparams(set);

cleanup:
    return (wifi_error)ret;
}

wifi_error wifi_initialize(wifi_handle *handle)
{
    int err = 0;
    bool driver_loaded = false;
    wifi_error ret = WIFI_SUCCESS;
    wifi_interface_handle iface_handle;
    srand(getpid());

    ALOGI("Initializing wifi");
    hal_info *info = (hal_info *)malloc(sizeof(hal_info));
    if (info == NULL) {
        ALOGE("Could not allocate hal_info");
        return WIFI_ERROR_UNKNOWN;
    }

    memset(info, 0, sizeof(*info));

    ALOGI("Creating socket");
    struct nl_sock *cmd_sock = wifi_create_nl_socket(WIFI_HAL_CMD_SOCK_PORT);
    if (cmd_sock == NULL) {
        ALOGE("Could not create handle");
        return WIFI_ERROR_UNKNOWN;
    }

    struct nl_sock *event_sock =
        wifi_create_nl_socket(WIFI_HAL_EVENT_SOCK_PORT);
    if (event_sock == NULL) {
        ALOGE("Could not create handle");
        nl_socket_free(cmd_sock);
        return WIFI_ERROR_UNKNOWN;
    }

    struct nl_cb *cb = nl_socket_get_cb(event_sock);
    if (cb == NULL) {
        ALOGE("Could not create handle");
        return WIFI_ERROR_UNKNOWN;
    }

    err = 1;
    nl_cb_set(cb, NL_CB_SEQ_CHECK, NL_CB_CUSTOM, no_seq_check, NULL);
    nl_cb_err(cb, NL_CB_CUSTOM, error_handler, &err);
    nl_cb_set(cb, NL_CB_FINISH, NL_CB_CUSTOM, finish_handler, &err);
    nl_cb_set(cb, NL_CB_ACK, NL_CB_CUSTOM, ack_handler, &err);

    nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, internal_valid_message_handler,
            info);
    nl_cb_put(cb);

    info->cmd_sock = cmd_sock;
    info->event_sock = event_sock;
    info->clean_up = false;
    info->in_event_loop = false;

    info->event_cb = (cb_info *)malloc(sizeof(cb_info) * DEFAULT_EVENT_CB_SIZE);
    info->alloc_event_cb = DEFAULT_EVENT_CB_SIZE;
    info->num_event_cb = 0;

    info->cmd = (cmd_info *)malloc(sizeof(cmd_info) * DEFAULT_CMD_SIZE);
    info->alloc_cmd = DEFAULT_CMD_SIZE;
    info->num_cmd = 0;

    info->nl80211_family_id = genl_ctrl_resolve(cmd_sock, "nl80211");
    if (info->nl80211_family_id < 0) {
        ALOGE("Could not resolve nl80211 familty id");
        nl_socket_free(cmd_sock);
        nl_socket_free(event_sock);
        free(info);
        return WIFI_ERROR_UNKNOWN;
    }
    ALOGI("%s: family_id:%d", __func__, info->nl80211_family_id);

    *handle = (wifi_handle) info;

    wifi_add_membership(*handle, "scan");
    wifi_add_membership(*handle, "mlme");
    wifi_add_membership(*handle, "regulatory");
    wifi_add_membership(*handle, "vendor");

    if (!is_wifi_driver_loaded()) {
        ret = (wifi_error)wifi_load_driver();
        if(ret != WIFI_SUCCESS) {
            ALOGE("%s Failed to load driver : %d\n", __func__, ret);
            return WIFI_ERROR_UNKNOWN;
        }
        driver_loaded = true;
    }

    ret = wifi_init_interfaces(*handle);
    if (ret != WIFI_SUCCESS) {
        ALOGI("Failed to init interfaces");
        goto unload;
    }

    if (info->num_interfaces == 0) {
        ALOGI("No interfaces found");
        ret = WIFI_ERROR_UNINITIALIZED;
        goto unload;
    }

    iface_handle = wifi_get_iface_handle((info->interfaces[0])->handle,
            (info->interfaces[0])->name);
    if (iface_handle == NULL) {
        int i;
        for (i = 0; i < info->num_interfaces; i++)
        {
            free(info->interfaces[i]);
        }
        ALOGE("%s no iface with %s\n", __func__, info->interfaces[0]->name);
        return WIFI_ERROR_UNKNOWN;
    }
    ret = acquire_supported_features(iface_handle,
            &info->supported_feature_set);
    if (ret != WIFI_SUCCESS) {
        ALOGI("Failed to get supported feature set : %d", ret);
        //acquire_supported_features failure is acceptable condition as legacy
        //drivers might not support the required vendor command. So, do not
        //consider it as failure of wifi_initialize
        ret = WIFI_SUCCESS;
    }

    ALOGI("Initialized Wifi HAL Successfully; vendor cmd = %d Supported"
            " features : %x", NL80211_CMD_VENDOR, info->supported_feature_set);

unload:
    if (driver_loaded)
        wifi_unload_driver();
    return ret;
}

static int wifi_add_membership(wifi_handle handle, const char *group)
{
    hal_info *info = getHalInfo(handle);

    int id = wifi_get_multicast_id(handle, "nl80211", group);
    if (id < 0) {
        ALOGE("Could not find group %s", group);
        return id;
    }

    int ret = nl_socket_add_membership(info->event_sock, id);
    if (ret < 0) {
        ALOGE("Could not add membership to group %s", group);
    }

    // ALOGI("Successfully added membership for group %s", group);
    return ret;
}

static void internal_cleaned_up_handler(wifi_handle handle)
{
    hal_info *info = getHalInfo(handle);
    wifi_cleaned_up_handler cleaned_up_handler = info->cleaned_up_handler;

    if (info->cmd_sock != 0) {
        nl_socket_free(info->cmd_sock);
        nl_socket_free(info->event_sock);
        info->cmd_sock = NULL;
        info->event_sock = NULL;
    }

    (*cleaned_up_handler)(handle);
    free(info);

    ALOGI("Internal cleanup completed");
}

void wifi_cleanup(wifi_handle handle, wifi_cleaned_up_handler handler)
{
    hal_info *info = getHalInfo(handle);
    info->cleaned_up_handler = handler;
    info->clean_up = true;

    ALOGI("Wifi cleanup completed");
}

static int internal_pollin_handler(wifi_handle handle)
{
    hal_info *info = getHalInfo(handle);
    struct nl_cb *cb = nl_socket_get_cb(info->event_sock);
    int res = nl_recvmsgs(info->event_sock, cb);
    if(res)
        ALOGE("Error :%d while reading nl msg", res);
    nl_cb_put(cb);
    return res;
}

static void internal_event_handler(wifi_handle handle, int events)
{
    if (events & POLLERR) {
        ALOGE("Error reading from socket");
        internal_pollin_handler(handle);
    } else if (events & POLLHUP) {
        ALOGE("Remote side hung up");
    } else if (events & POLLIN) {
        ALOGI("Found some events!!!");
        internal_pollin_handler(handle);
    } else {
        ALOGE("Unknown event - %0x", events);
    }
}

/* Run event handler */
void wifi_event_loop(wifi_handle handle)
{
    hal_info *info = getHalInfo(handle);
    if (info->in_event_loop) {
        return;
    } else {
        info->in_event_loop = true;
    }

    pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));

    pfd.fd = nl_socket_get_fd(info->event_sock);
    pfd.events = POLLIN;

    /* TODO: Add support for timeouts */

    do {
        int timeout = -1;                   /* Infinite timeout */
        pfd.revents = 0;
        //ALOGI("Polling socket");
        int result = poll(&pfd, 1, -1);
        ALOGI("Poll result = %0x", result);
        if (result < 0) {
            ALOGE("Error polling socket");
        } else if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
            internal_event_handler(handle, pfd.revents);
        }
    } while (!info->clean_up);


    ALOGI("Cleaning up");
    internal_cleaned_up_handler(handle);
}

////////////////////////////////////////////////////////////////////////////////

static int internal_valid_message_handler(nl_msg *msg, void *arg)
{
    wifi_handle handle = (wifi_handle)arg;
    hal_info *info = getHalInfo(handle);

    WifiEvent event(msg);
    int res = event.parse();
    if (res < 0) {
        ALOGE("Failed to parse event: %d", res);
        return NL_SKIP;
    }

    int cmd = event.get_cmd();
    uint32_t vendor_id = 0;
    int subcmd = 0;

    if (cmd == NL80211_CMD_VENDOR) {
        vendor_id = event.get_u32(NL80211_ATTR_VENDOR_ID);
        subcmd = event.get_u32(NL80211_ATTR_VENDOR_SUBCMD);
        ALOGI("event received %s, vendor_id = 0x%0x, subcmd = 0x%0x",
                event.get_cmdString(), vendor_id, subcmd);
    } else {
        ALOGI("event received %s", event.get_cmdString());
    }

    ALOGI("event received %s, vendor_id = 0x%0x", event.get_cmdString(),
            vendor_id);
    // event.log();

    bool dispatched = false;
    for (int i = 0; i < info->num_event_cb; i++) {
        if (cmd == info->event_cb[i].nl_cmd) {
            if (cmd == NL80211_CMD_VENDOR
                && ((vendor_id != info->event_cb[i].vendor_id)
                || (subcmd != info->event_cb[i].vendor_subcmd)))
            {
                /* event for a different vendor, ignore it */
                continue;
            }

            cb_info *cbi = &(info->event_cb[i]);
            (*(cbi->cb_func))(msg, cbi->cb_arg);
            dispatched = true;
        }
    }

    if (!dispatched) {
        ALOGI("event ignored!!");
    }

    return NL_OK;
}

////////////////////////////////////////////////////////////////////////////////

class GetMulticastIdCommand : public WifiCommand
{
private:
    const char *mName;
    const char *mGroup;
    int   mId;
public:
    GetMulticastIdCommand(wifi_handle handle, const char *name,
            const char *group) : WifiCommand(handle, 0)
    {
        mName = name;
        mGroup = group;
        mId = -1;
    }

    int getId() {
        return mId;
    }

    virtual int create() {
        int nlctrlFamily = genl_ctrl_resolve(mInfo->cmd_sock, "nlctrl");
        // ALOGI("ctrl family = %d", nlctrlFamily);
        int ret = mMsg.create(nlctrlFamily, CTRL_CMD_GETFAMILY, 0, 0);
        if (ret < 0) {
            return ret;
        }
        ret = mMsg.put_string(CTRL_ATTR_FAMILY_NAME, mName);
        return ret;
    }

    virtual int handleResponse(WifiEvent& reply) {

        // ALOGI("handling reponse in %s", __func__);

        struct nlattr **tb = reply.attributes();
        struct genlmsghdr *gnlh = reply.header();
        struct nlattr *mcgrp = NULL;
        int i;

        if (!tb[CTRL_ATTR_MCAST_GROUPS]) {
            ALOGI("No multicast groups found");
            return NL_SKIP;
        } else {
            // ALOGI("Multicast groups attr size = %d",
            // nla_len(tb[CTRL_ATTR_MCAST_GROUPS]));
        }

        for_each_attr(mcgrp, tb[CTRL_ATTR_MCAST_GROUPS], i) {

            // ALOGI("Processing group");
            struct nlattr *tb2[CTRL_ATTR_MCAST_GRP_MAX + 1];
            nla_parse(tb2, CTRL_ATTR_MCAST_GRP_MAX, (nlattr *)nla_data(mcgrp),
                nla_len(mcgrp), NULL);
            if (!tb2[CTRL_ATTR_MCAST_GRP_NAME] || !tb2[CTRL_ATTR_MCAST_GRP_ID])
            {
                continue;
            }

            char *grpName = (char *)nla_data(tb2[CTRL_ATTR_MCAST_GRP_NAME]);
            int grpNameLen = nla_len(tb2[CTRL_ATTR_MCAST_GRP_NAME]);

            // ALOGI("Found group name %s", grpName);

            if (strncmp(grpName, mGroup, grpNameLen) != 0)
                continue;

            mId = nla_get_u32(tb2[CTRL_ATTR_MCAST_GRP_ID]);
            break;
        }

        return NL_SKIP;
    }

};

static int wifi_get_multicast_id(wifi_handle handle, const char *name,
        const char *group)
{
    GetMulticastIdCommand cmd(handle, name, group);
    int res = cmd.requestResponse();
    if (res < 0)
        return res;
    else
        return cmd.getId();
}

/////////////////////////////////////////////////////////////////////////

static bool is_wifi_interface(const char *name)
{
    if (strncmp(name, "wlan", 4) != 0 && strncmp(name, "p2p", 3) != 0) {
        /* not a wifi interface; ignore it */
        return false;
    } else {
        return true;
    }
}

static int get_interface(const char *name, interface_info *info)
{
    strlcpy(info->name, name, (IFNAMSIZ + 1));
    info->id = if_nametoindex(name);
    // ALOGI("found an interface : %s, id = %d", name, info->id);
    return WIFI_SUCCESS;
}

wifi_error wifi_init_interfaces(wifi_handle handle)
{
    hal_info *info = (hal_info *)handle;

    struct dirent *de;

    DIR *d = opendir("/sys/class/net");
    if (d == 0)
        return WIFI_ERROR_UNKNOWN;

    int n = 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.')
            continue;
        if (is_wifi_interface(de->d_name) ) {
            n++;
        }
    }

    closedir(d);

    d = opendir("/sys/class/net");
    if (d == 0)
        return WIFI_ERROR_UNKNOWN;

    info->interfaces = (interface_info **)malloc(sizeof(interface_info *) * n);
    if (info->interfaces == NULL) {
        ALOGE("%s: Error info->interfaces NULL", __func__);
        return WIFI_ERROR_OUT_OF_MEMORY;
    }

    int i = 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.')
            continue;
        if (is_wifi_interface(de->d_name)) {
            interface_info *ifinfo
                = (interface_info *)malloc(sizeof(interface_info));
            if (ifinfo == NULL) {
                ALOGE("%s: Error ifinfo NULL", __func__);
                while (i > 0) {
                    free(info->interfaces[i-1]);
                    i--;
                }
                free(info->interfaces);
                return WIFI_ERROR_OUT_OF_MEMORY;
            }
            if (get_interface(de->d_name, ifinfo) != WIFI_SUCCESS) {
                free(ifinfo);
                continue;
            }
            ifinfo->handle = handle;
            info->interfaces[i] = ifinfo;
            i++;
        }
    }

    closedir(d);

    info->num_interfaces = n;
    ALOGI("Found %d interfaces", info->num_interfaces);

    return WIFI_SUCCESS;
}

wifi_error wifi_get_ifaces(wifi_handle handle, int *num,
        wifi_interface_handle **interfaces)
{
    hal_info *info = (hal_info *)handle;

    *interfaces = (wifi_interface_handle *)info->interfaces;
    *num = info->num_interfaces;

    return WIFI_SUCCESS;
}

wifi_error wifi_get_iface_name(wifi_interface_handle handle, char *name,
        size_t size)
{
    interface_info *info = (interface_info *)handle;
    strlcpy(name, info->name, size);
    return WIFI_SUCCESS;
}

/* Get the supported Feature set */
wifi_error wifi_get_supported_feature_set(wifi_interface_handle iface,
        feature_set *set)
{
    int ret = 0;
    wifi_handle handle = getWifiHandle(iface);
    *set = 0;
    hal_info *info = getHalInfo(handle);

    ret = acquire_supported_features(iface, set);
    if (ret != WIFI_SUCCESS) {
        *set = info->supported_feature_set;
        ALOGI("Supported feature set acquired at initialization : %x", *set);
    } else {
        info->supported_feature_set = *set;
        ALOGI("Supported feature set acquired : %x", *set);
    }
    return WIFI_SUCCESS;
}

wifi_error wifi_get_concurrency_matrix(wifi_interface_handle handle,
                                       int set_size_max,
                                       feature_set set[], int *set_size)
{
    int ret = 0;
    struct nlattr *nlData;
    WifihalGeneric *vCommand = NULL;
    interface_info *ifaceInfo = getIfaceInfo(handle);
    wifi_handle wifiHandle = getWifiHandle(handle);

    if (set == NULL) {
        ALOGE("%s: NULL set pointer provided. Exit.",
            __func__);
        return WIFI_ERROR_INVALID_ARGS;
    }

    vCommand = new WifihalGeneric(wifiHandle, 0,
            OUI_QCA,
            QCA_NL80211_VENDOR_SUBCMD_GET_CONCURRENCY_MATRIX);
    if (vCommand == NULL) {
        ALOGE("%s: Error vCommand NULL", __func__);
        return WIFI_ERROR_OUT_OF_MEMORY;
    }

    /* Create the message */
    ret = vCommand->create();
    if (ret < 0)
        goto cleanup;

    ret = vCommand->set_iface_id(ifaceInfo->name);
    if (ret < 0)
        goto cleanup;

    /* Add the vendor specific attributes for the NL command. */
    nlData = vCommand->attr_start(NL80211_ATTR_VENDOR_DATA);
    if (!nlData)
        goto cleanup;

    if (vCommand->put_u32(
        QCA_WLAN_VENDOR_ATTR_GET_CONCURRENCY_MATRIX_CONFIG_PARAM_SET_SIZE_MAX,
        set_size_max))
    {
        goto cleanup;
    }
    vCommand->attr_end(nlData);

    /* Populate the input received from caller/framework. */
    vCommand->setMaxSetSize(set_size_max);
    vCommand->setSizePtr(set_size);
    vCommand->setConcurrencySet(set);

    ret = vCommand->requestResponse();
    if (ret) {
        ALOGE("%s: requestResponse() error: %d", __func__, ret);
    }

cleanup:
    ALOGI("%s: Delete object.", __func__);
    delete vCommand;
    if (ret) {
        *set_size = 0;
    }
    return (wifi_error)ret;
}


wifi_error wifi_set_nodfs_flag(wifi_interface_handle handle, u32 nodfs)
{
    int ret = 0;
    struct nlattr *nlData;
    WifiVendorCommand *vCommand = NULL;
    interface_info *ifaceInfo = getIfaceInfo(handle);
    wifi_handle wifiHandle = getWifiHandle(handle);

    vCommand = new WifiVendorCommand(wifiHandle, 0,
            OUI_QCA,
            QCA_NL80211_VENDOR_SUBCMD_NO_DFS_FLAG);
    if (vCommand == NULL) {
        ALOGE("%s: Error vCommand NULL", __func__);
        return WIFI_ERROR_OUT_OF_MEMORY;
    }

    /* Create the message */
    ret = vCommand->create();
    if (ret < 0)
        goto cleanup;

    ret = vCommand->set_iface_id(ifaceInfo->name);
    if (ret < 0)
        goto cleanup;

    /* Add the vendor specific attributes for the NL command. */
    nlData = vCommand->attr_start(NL80211_ATTR_VENDOR_DATA);
    if (!nlData)
        goto cleanup;

    /* Add the fixed part of the mac_oui to the nl command */
    if (vCommand->put_u32(QCA_WLAN_VENDOR_ATTR_SET_NO_DFS_FLAG, nodfs)) {
        goto cleanup;
    }

    vCommand->attr_end(nlData);

    ret = vCommand->requestResponse();
    /* Don't check response since we aren't expecting one */

cleanup:
    delete vCommand;
    return (wifi_error)ret;
}
