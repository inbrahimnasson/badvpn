/**
 * @file net_backend_wpa_supplicant.c
 * @author Ambroz Bizjak <ambrop7@gmail.com>
 * 
 * @section LICENSE
 * 
 * This file is part of BadVPN.
 * 
 * BadVPN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 * 
 * BadVPN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * 
 * @section DESCRIPTION
 * 
 * Wireless interface module which runs wpa_supplicant to connect to a wireless network.
 * 
 * Note: wpa_supplicant does not monitor the state of rfkill switches and will fail to
 * start if the switch is of when it is started, and will stop working indefinitely if the
 * switch is turned off while it is running. Therefore, you should put a "net.backend.rfkill"
 * statement in front of the wpa_supplicant statement.
 * 
 * Synopsis: net.backend.wpa_supplicant(string ifname, string conf, string exec, list(string) args)
 * Variables:
 *   bssid - BSSID of the wireless network we connected to, or "none".
 *           Consists of 6 capital, two-character hexadecimal numbers, separated with colons.
 *           Example: "01:B2:C3:04:E5:F6"
 *   ssid - SSID of the wireless network we connected to. Note that this is after what
 *          wpa_supplicant does to it before it prints it. In particular, it replaces all bytes
 *          outside [32, 126] with underscores.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include <misc/cmdline.h>
#include <misc/string_begins_with.h>
#include <misc/stdbuf_cmdline.h>
#include <misc/balloc.h>
#include <flow/LineBuffer.h>
#include <system/BInputProcess.h>
#include <ncd/NCDModule.h>

#include <generated/blog_channel_ncd_net_backend_wpa_supplicant.h>

#define MAX_LINE_LEN 512
#define EVENT_STRING_CONNECTED "CTRL-EVENT-CONNECTED"
#define EVENT_STRING_DISCONNECTED "CTRL-EVENT-DISCONNECTED"

#define ModuleLog(i, ...) NCDModuleInst_Backend_Log((i), BLOG_CURRENT_CHANNEL, __VA_ARGS__)

struct instance {
    NCDModuleInst *i;
    char *ifname;
    char *conf;
    char *exec;
    NCDValue *args;
    int dying;
    int up;
    BInputProcess process;
    int have_pipe;
    LineBuffer pipe_buffer;
    PacketPassInterface pipe_input;
    int have_info;
    int info_have_bssid;
    uint8_t info_bssid[6];
    char *info_ssid;
};

static int parse_hex_digit (uint8_t d, uint8_t *out);
static int parse_trying (uint8_t *data, int data_len, uint8_t *out_bssid, uint8_t **out_ssid, int *out_ssid_len);
static int parse_trying_nobssid (uint8_t *data, int data_len, uint8_t **out_ssid, int *out_ssid_len);
static int build_cmdline (struct instance *o, CmdLine *c);
static int init_info (struct instance *o, int have_bssid, const uint8_t *bssid, const uint8_t *ssid, size_t ssid_len);
static void free_info (struct instance *o);
static void process_error (struct instance *o);
static void process_handler_terminated (struct instance *o, int normally, uint8_t normally_exit_status);
static void process_handler_closed (struct instance *o, int is_error);
static void process_pipe_handler_send (struct instance *o, uint8_t *data, int data_len);
static void instance_free (struct instance *o);

int parse_hex_digit (uint8_t d, uint8_t *out)
{
    switch (d) {
        case '0': *out = 0; return 1;
        case '1': *out = 1; return 1;
        case '2': *out = 2; return 1;
        case '3': *out = 3; return 1;
        case '4': *out = 4; return 1;
        case '5': *out = 5; return 1;
        case '6': *out = 6; return 1;
        case '7': *out = 7; return 1;
        case '8': *out = 8; return 1;
        case '9': *out = 9; return 1;
        case 'A': case 'a': *out = 10; return 1;
        case 'B': case 'b': *out = 11; return 1;
        case 'C': case 'c': *out = 12; return 1;
        case 'D': case 'd': *out = 13; return 1;
        case 'E': case 'e': *out = 14; return 1;
        case 'F': case 'f': *out = 15; return 1;
    }
    
    return 0;
}

int parse_trying (uint8_t *data, int data_len, uint8_t *out_bssid, uint8_t **out_ssid, int *out_ssid_len)
{
    // Trying to associate with AB:CD:EF:01:23:45 (SSID='Some SSID' freq=2462 MHz)
    
    int p;
    if (!(p = data_begins_with((char *)data, data_len, "Trying to associate with "))) {
        return 0;
    }
    data += p;
    data_len -= p;
    
    for (int i = 0; i < 6; i++) {
        uint8_t d1;
        uint8_t d2;
        if (data_len < 2 || !parse_hex_digit(data[0], &d1) || !parse_hex_digit(data[1], &d2)) {
            return 0;
        }
        data += 2;
        data_len -= 2;
        out_bssid[i] = ((d1 << 4) | d2);
        
        if (i != 5) {
            if (data_len < 1 || data[0] != ':') {
                return 0;
            }
            data += 1;
            data_len -= 1;
        }
    }
    
    if (!(p = data_begins_with((char *)data, data_len, " (SSID='"))) {
        return 0;
    }
    data += p;
    data_len -= p;
    
    // find last '
    uint8_t *q = NULL;
    for (int i = data_len; i > 0; i--) {
        if (data[i - 1] == '\'') {
            q = &data[i - 1];
            break;
        }
    }
    if (!q) {
        return 0;
    }
    
    *out_ssid = data;
    *out_ssid_len = q - data;
    
    return 1;
}

int parse_trying_nobssid (uint8_t *data, int data_len, uint8_t **out_ssid, int *out_ssid_len)
{
    // Trying to associate with SSID 'Some SSID'
    
    int p;
    if (!(p = data_begins_with((char *)data, data_len, "Trying to associate with SSID '"))) {
        return 0;
    }
    data += p;
    data_len -= p;
    
    // find last '
    uint8_t *q = NULL;
    for (int i = data_len; i > 0; i--) {
        if (data[i - 1] == '\'') {
            q = &data[i - 1];
            break;
        }
    }
    if (!q) {
        return 0;
    }
    
    *out_ssid = data;
    *out_ssid_len = q - data;
    
    return 1;
}

int build_cmdline (struct instance *o, CmdLine *c)
{
    // init cmdline
    if (!CmdLine_Init(c)) {
        goto fail0;
    }
    
    // append stdbuf part
    if (!build_stdbuf_cmdline(c, o->exec)) {
        goto fail1;
    }
    
    // append user arguments
    NCDValue *arg = NCDValue_ListFirst(o->args);
    while (arg) {
        if (NCDValue_Type(arg) != NCDVALUE_STRING) {
            ModuleLog(o->i, BLOG_ERROR, "wrong type");
            goto fail1;
        }
        
        // append argument
        if (!CmdLine_Append(c, NCDValue_StringValue(arg))) {
            goto fail1;
        }
        
        arg = NCDValue_ListNext(o->args, arg);
    }
    
    // append interface name
    if (!CmdLine_Append(c, "-i") || !CmdLine_Append(c, o->ifname)) {
        goto fail1;
    }
    
    // append config file
    if (!CmdLine_Append(c, "-c") || !CmdLine_Append(c, o->conf)) {
        goto fail1;
    }
    
    // terminate cmdline
    if (!CmdLine_Finish(c)) {
        goto fail1;
    }
    
    return 1;
    
fail1:
    CmdLine_Free(c);
fail0:
    return 0;
}

int init_info (struct instance *o, int have_bssid, const uint8_t *bssid, const uint8_t *ssid, size_t ssid_len)
{
    ASSERT(!o->have_info)
    
    // set bssid
    o->info_have_bssid = have_bssid;
    if (have_bssid) {
        memcpy(o->info_bssid, bssid, 6);
    }
    
    // set ssid
    if (!(o->info_ssid = BAllocSize(bsize_add(bsize_fromsize(ssid_len), bsize_fromsize(1))))) {
        ModuleLog(o->i, BLOG_ERROR, "BAllocSize failed");
        return 0;
    }
    memcpy(o->info_ssid, ssid, ssid_len);
    o->info_ssid[ssid_len] = '\0';
    
    // set have info
    o->have_info = 1;
    
    return 1;
}

void free_info (struct instance *o)
{
    ASSERT(o->have_info)
    
    // free ssid
    BFree(o->info_ssid);
    
    // set not have info
    o->have_info = 0;
}

void process_error (struct instance *o)
{
    BInputProcess_Terminate(&o->process);
}

void process_handler_terminated (struct instance *o, int normally, uint8_t normally_exit_status)
{
    ModuleLog(o->i, (o->dying ? BLOG_INFO : BLOG_ERROR), "process terminated");
    
    if (!o->dying) {
        NCDModuleInst_Backend_SetError(o->i);
    }
    
    // die
    instance_free(o);
    return;
}

void process_handler_closed (struct instance *o, int is_error)
{
    ASSERT(o->have_pipe)
    
    if (is_error) {
        ModuleLog(o->i, BLOG_ERROR, "pipe error");
    } else {
        ModuleLog(o->i, BLOG_INFO, "pipe closed");
    }
    
    // free buffer
    LineBuffer_Free(&o->pipe_buffer);
    
    // free input interface
    PacketPassInterface_Free(&o->pipe_input);
    
    // set have no pipe
    o->have_pipe = 0;
}

void process_pipe_handler_send (struct instance *o, uint8_t *data, int data_len)
{
    ASSERT(o->have_pipe)
    ASSERT(data_len > 0)
    
    // accept packet
    PacketPassInterface_Done(&o->pipe_input);
    
    if (o->dying) {
        return;
    }
    
    int have_bssid = 1;
    uint8_t bssid[6];
    uint8_t *ssid;
    int ssid_len;
    if (parse_trying(data, data_len, bssid, &ssid, &ssid_len) || (have_bssid = 0, parse_trying_nobssid(data, data_len, &ssid, &ssid_len))) {
        ModuleLog(o->i, BLOG_INFO, "trying event");
        
        if (o->up) {
            ModuleLog(o->i, BLOG_ERROR, "trying unexpected!");
            process_error(o);
            return;
        }
        
        if (o->have_info) {
            free_info(o);
        }
        
        if (!init_info(o, have_bssid, bssid, ssid, ssid_len)) {
            ModuleLog(o->i, BLOG_ERROR, "init_info failed");
            process_error(o);
            return;
        }
    }
    else if (data_begins_with((char *)data, data_len, EVENT_STRING_CONNECTED)) {
        ModuleLog(o->i, BLOG_INFO, "connected event");
        
        if (o->up || !o->have_info) {
            ModuleLog(o->i, BLOG_ERROR, "connected unexpected!");
            process_error(o);
            return;
        }
        
        o->up = 1;
        NCDModuleInst_Backend_Up(o->i);
    }
    else if (data_begins_with((char *)data, data_len, EVENT_STRING_DISCONNECTED)) {
        ModuleLog(o->i, BLOG_INFO, "disconnected event");
        
        if (o->have_info) {
            free_info(o);
        }
        
        if (o->up) {
            o->up = 0;
            NCDModuleInst_Backend_Down(o->i);
        }
    }
}

static void func_new (NCDModuleInst *i)
{
    // allocate instance
    struct instance *o = malloc(sizeof(*o));
    if (!o) {
        ModuleLog(i, BLOG_ERROR, "failed to allocate instance");
        goto fail0;
    }
    NCDModuleInst_Backend_SetUser(i, o);
    
    // init arguments
    o->i = i;
    
    // read arguments
    NCDValue *ifname_arg;
    NCDValue *conf_arg;
    NCDValue *exec_arg;
    NCDValue *args_arg;
    if (!NCDValue_ListRead(o->i->args, 4, &ifname_arg, &conf_arg, &exec_arg, &args_arg)) {
        ModuleLog(o->i, BLOG_ERROR, "wrong arity");
        goto fail1;
    }
    if (NCDValue_Type(ifname_arg) != NCDVALUE_STRING || NCDValue_Type(conf_arg) != NCDVALUE_STRING ||
        NCDValue_Type(exec_arg) != NCDVALUE_STRING || NCDValue_Type(args_arg) != NCDVALUE_LIST) {
        ModuleLog(o->i, BLOG_ERROR, "wrong type");
        goto fail1;
    }
    o->ifname = NCDValue_StringValue(ifname_arg);
    o->conf = NCDValue_StringValue(conf_arg);
    o->exec = NCDValue_StringValue(exec_arg);
    o->args = args_arg;
    
    // set not dying
    o->dying = 0;
    
    // set not up
    o->up = 0;
    
    // build process cmdline
    CmdLine c;
    if (!build_cmdline(o, &c)) {
        ModuleLog(o->i, BLOG_ERROR, "failed to build cmdline");
        goto fail1;
    }
    
    // init process
    if (!BInputProcess_Init(&o->process, o->i->reactor, o->i->manager, o,
                            (BInputProcess_handler_terminated)process_handler_terminated,
                            (BInputProcess_handler_closed)process_handler_closed
    )) {
        ModuleLog(o->i, BLOG_ERROR, "BInputProcess_Init failed");
        goto fail2;
    }
    
    // init input interface
    PacketPassInterface_Init(&o->pipe_input, MAX_LINE_LEN, (PacketPassInterface_handler_send)process_pipe_handler_send, o, BReactor_PendingGroup(o->i->reactor));
    
    // init buffer
    if (!LineBuffer_Init(&o->pipe_buffer, BInputProcess_GetInput(&o->process), &o->pipe_input, MAX_LINE_LEN, '\n')) {
        ModuleLog(o->i, BLOG_ERROR, "LineBuffer_Init failed");
        goto fail3;
    }
    
    // set have pipe
    o->have_pipe = 1;
    
    // start process
    if (!BInputProcess_Start(&o->process, ((char **)c.arr.v)[0], (char **)c.arr.v, NULL)) {
        ModuleLog(o->i, BLOG_ERROR, "BInputProcess_Start failed");
        goto fail4;
    }
    
    // set not have info
    o->have_info = 0;
    
    CmdLine_Free(&c);
    
    return;
    
fail4:
    LineBuffer_Free(&o->pipe_buffer);
fail3:
    PacketPassInterface_Free(&o->pipe_input);
    BInputProcess_Free(&o->process);
fail2:
    CmdLine_Free(&c);
fail1:
    free(o);
fail0:
    NCDModuleInst_Backend_SetError(i);
    NCDModuleInst_Backend_Dead(i);
}

void instance_free (struct instance *o)
{
    NCDModuleInst *i = o->i;
    
    // free info
    if (o->have_info) {
        free_info(o);
    }
    
    if (o->have_pipe) {
        // free buffer
        LineBuffer_Free(&o->pipe_buffer);
        
        // free input interface
        PacketPassInterface_Free(&o->pipe_input);
    }
    
    // free process
    BInputProcess_Free(&o->process);
    
    // free instance
    free(o);
    
    NCDModuleInst_Backend_Dead(i);
}

static void func_die (void *vo)
{
    struct instance *o = vo;
    ASSERT(!o->dying)
    
    // request termination
    BInputProcess_Terminate(&o->process);
    
    // remember dying
    o->dying = 1;
}

static int func_getvar (void *vo, const char *name, NCDValue *out)
{
    struct instance *o = vo;
    ASSERT(o->up)
    ASSERT(o->have_info)
    
    if (!strcmp(name, "bssid")) {
        char str[18];
        
        if (!o->info_have_bssid) {
            sprintf(str, "none");
        } else {
            uint8_t *id = o->info_bssid;
            sprintf(str, "%02"PRIX8":%02"PRIX8":%02"PRIX8":%02"PRIX8":%02"PRIX8":%02"PRIX8, id[0], id[1], id[2], id[3], id[4], id[5]);
        }
        
        if (!NCDValue_InitString(out, str)) {
            ModuleLog(o->i, BLOG_ERROR, "NCDValue_InitString failed");
            return 0;
        }
        
        return 1;
    }
    
    if (!strcmp(name, "ssid")) {
        if (!NCDValue_InitString(out, o->info_ssid)) {
            ModuleLog(o->i, BLOG_ERROR, "NCDValue_InitString failed");
            return 0;
        }
        
        return 1;
    }
    
    return 0;
}

static const struct NCDModule modules[] = {
    {
        .type = "net.backend.wpa_supplicant",
        .func_new = func_new,
        .func_die = func_die,
        .func_getvar = func_getvar
    }, {
        .type = NULL
    }
};

const struct NCDModuleGroup ncdmodule_net_backend_wpa_supplicant = {
    .modules = modules
};
