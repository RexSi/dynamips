/*
 * Cisco 3745 simulation platform.
 * Copyright (c) 2006 Christophe Fillot (cf@utc.fr)
 *
 * Hypervisor C3745 routines.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>

#include "cpu.h"
#include "vm.h"
#include "device.h"
#include "dev_c3745.h"
#include "dev_vtty.h"
#include "utils.h"
#include "net.h"
#include "atm.h"
#include "frame_relay.h"
#include "crc.h"
#include "net_io.h"
#include "net_io_bridge.h"
#ifdef GEN_ETH
#include "gen_eth.h"
#endif
#include "registry.h"
#include "hypervisor.h"

/* Create a C3745 instance */
static int cmd_create(hypervisor_conn_t *conn,int argc,char *argv[])
{
   c3745_t *router;

   if (!(router = c3745_create_instance(argv[0],atoi(argv[1])))) {
      hypervisor_send_reply(conn,HSC_ERR_CREATE,1,
                            "unable to create C3745 instance '%s'",
                            argv[0]);
      return(-1);
   }

   router->vm->vtty_con_type = VTTY_TYPE_NONE;
   router->vm->vtty_aux_type = VTTY_TYPE_NONE;
   
   vm_release(router->vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"C3745 '%s' created",argv[0]);
   return(0);
}

/* Delete a C3745 instance */
static int cmd_delete(hypervisor_conn_t *conn,int argc,char *argv[])
{
   int res;

   res = c3745_delete_instance(argv[0]);

   if (res == 1) {
      hypervisor_send_reply(conn,HSC_INFO_OK,1,"C3745 '%s' deleted",argv[0]);
   } else {
      hypervisor_send_reply(conn,HSC_ERR_DELETE,1,
                            "unable to delete C3745 '%s'",argv[0]);
   }

   return(res);
}

/* Set the I/O mem size */
static int cmd_set_iomem(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;

   if (!(vm = hypervisor_find_vm(conn,argv[0],VM_TYPE_C3745)))
      return(-1);

   VM_C3745(vm)->nm_iomem_size = 0x8000 | atoi(optarg);

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Set the base MAC address for the chassis */
static int cmd_set_mac_addr(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;

   if (!(vm = hypervisor_find_vm(conn,argv[0],VM_TYPE_C3745)))
      return(-1);

   if ((c3745_chassis_set_mac_addr(VM_C3745(vm),argv[1])) == -1) {
      vm_release(vm);
      hypervisor_send_reply(conn,HSC_ERR_CREATE,1,
                            "unable to set MAC address for router '%s'",
                            argv[0]);
      return(-1);
   }

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Start a C3745 instance */
static int cmd_start(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;
   c3745_t *router;

   if (!(vm = hypervisor_find_vm(conn,argv[0],VM_TYPE_C3745)))
      return(-1);

   router = VM_C3745(vm);

   if (router->vm->vtty_con_type == VTTY_TYPE_NONE) {
      hypervisor_send_reply(conn,HSC_INFO_MSG,0,
                            "Warning: no console port defined for "
                            "C3745 '%s'",argv[0]);
   }

   if (c3745_init_instance(router) == -1) {
      vm_release(vm);
      hypervisor_send_reply(conn,HSC_ERR_START,1,
                            "unable to start instance '%s'",
                            argv[0]);
      return(-1);
   }
   
   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"C3745 '%s' started",argv[0]);
   return(0);
}

/* Stop a C3745 instance */
static int cmd_stop(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;
   c3745_t *router;

   if (!(vm = hypervisor_find_vm(conn,argv[0],VM_TYPE_C3745)))
      return(-1);

   router = VM_C3745(vm);

   if (c3745_stop_instance(router) == -1) {
      vm_release(vm);
      hypervisor_send_reply(conn,HSC_ERR_STOP,1,
                            "unable to stop instance '%s'",
                            argv[0]);
      return(-1);
   }
   
   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"C3745 '%s' stopped",argv[0]);
   return(0);
}

/* Show NM bindings */
static int cmd_nm_bindings(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;
   c3745_t *router;
   char *nm_type;
   int i;

   if (!(vm = hypervisor_find_vm(conn,argv[0],VM_TYPE_C3745)))
      return(-1);

   router = VM_C3745(vm);

   for(i=0;i<C3745_MAX_NM_BAYS;i++) {
      nm_type = c3745_nm_get_type(router,i);
      if (nm_type)
         hypervisor_send_reply(conn,HSC_INFO_MSG,0,"%u: %s",i,nm_type);
   }
   
   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Show NM NIO bindings */
static int cmd_nm_nio_bindings(hypervisor_conn_t *conn,int argc,char *argv[])
{
   struct c3745_nio_binding *nb;
   struct c3745_nm_bay *bay;
   vm_instance_t *vm;
   c3745_t *router;
   u_int nm_bay;

   if (!(vm = hypervisor_find_vm(conn,argv[0],VM_TYPE_C3745)))
      return(-1);

   router = VM_C3745(vm);
   nm_bay = atoi(argv[1]);

   if (!(bay = c3745_nm_get_info(router,nm_bay))) {
      vm_release(vm);
      hypervisor_send_reply(conn,HSC_ERR_UNK_OBJ,1,"Invalid slot %u",nm_bay);
      return(-1);
   }

   for(nb=bay->nio_list;nb;nb=nb->next)
      hypervisor_send_reply(conn,HSC_INFO_MSG,0,"%u: %s",
                            nb->port_id,nb->nio->name);
   
   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Add a NM binding for the specified slot */
static int cmd_add_nm_binding(hypervisor_conn_t *conn,int argc,char *argv[])
{   
   vm_instance_t *vm;
   c3745_t *router;
   u_int nm_bay;

   if (!(vm = hypervisor_find_vm(conn,argv[0],VM_TYPE_C3745)))
      return(-1);

   router = VM_C3745(vm);
   nm_bay = atoi(argv[1]);

   if (c3745_nm_add_binding(router,argv[2],nm_bay) == -1) {
      vm_release(vm);
      hypervisor_send_reply(conn,HSC_ERR_BINDING,1,
                            "C3745 %s: unable to add NM binding for slot %u",
                            argv[0],nm_bay);
      return(-1);
   }

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Remove a NM binding for the specified slot */
static int cmd_remove_nm_binding(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;
   c3745_t *router;
   u_int nm_bay;

   if (!(vm = hypervisor_find_vm(conn,argv[0],VM_TYPE_C3745)))
      return(-1);

   router = VM_C3745(vm);
   nm_bay = atoi(argv[1]);

   if (c3745_nm_remove_binding(router,nm_bay) == -1) {
      vm_release(vm);
      hypervisor_send_reply(conn,HSC_ERR_BINDING,1,
                            "C3745 %s: unable to remove NM binding for "
                            "slot %u",argv[0],nm_bay);
      return(-1);
   }

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Add a NIO binding to the specified slot/port */
static int cmd_add_nio_binding(hypervisor_conn_t *conn,int argc,char *argv[])
{  
   u_int nm_bay,port_id;
   vm_instance_t *vm;
   c3745_t *router;

   if (!(vm = hypervisor_find_vm(conn,argv[0],VM_TYPE_C3745)))
      return(-1);

   router = VM_C3745(vm);

   nm_bay = atoi(argv[1]);
   port_id = atoi(argv[2]);

   if (c3745_nm_add_nio_binding(router,nm_bay,port_id,argv[3]) == -1) {
      vm_release(vm);
      hypervisor_send_reply(conn,HSC_ERR_BINDING,1,
                            "C3745 %s: unable to add NIO binding for "
                            "interface %u/%u",argv[0],nm_bay,port_id);
      return(-1);
   }

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Remove a NIO binding from the specified slot/port */
static int cmd_remove_nio_binding(hypervisor_conn_t *conn,
                                  int argc,char *argv[])
{
   u_int nm_bay,port_id;
   vm_instance_t *vm;
   c3745_t *router;

   if (!(vm = hypervisor_find_vm(conn,argv[0],VM_TYPE_C3745)))
      return(-1);

   router = VM_C3745(vm);

   nm_bay = atoi(argv[1]);
   port_id = atoi(argv[2]);

   if (c3745_nm_remove_nio_binding(router,nm_bay,port_id) == -1) {
      vm_release(vm);
      hypervisor_send_reply(conn,HSC_ERR_BINDING,1,
                            "C3745 %s: unable to remove NIO binding for "
                            "interface %u/%u",argv[0],nm_bay,port_id);
      return(-1);
   }

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Enable NIO of the specified slot/port */
static int cmd_nm_enable_nio(hypervisor_conn_t *conn,int argc,char *argv[])
{  
   u_int nm_bay,port_id;
   vm_instance_t *vm;
   c3745_t *router;

   if (!(vm = hypervisor_find_vm(conn,argv[0],VM_TYPE_C3745)))
      return(-1);

   router = VM_C3745(vm);

   nm_bay = atoi(argv[1]);
   port_id = atoi(argv[2]);

   if (c3745_nm_enable_nio(router,nm_bay,port_id) == -1) {
      vm_release(vm);
      hypervisor_send_reply(conn,HSC_ERR_BINDING,1,
                            "C3745 %s: unable to enable NIO for "
                            "interface %u/%u",argv[0],nm_bay,port_id);
      return(-1);
   }

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Disable NIO of the specified slot/port */
static int cmd_nm_disable_nio(hypervisor_conn_t *conn,int argc,char *argv[])
{  
   u_int nm_bay,port_id;
   vm_instance_t *vm;
   c3745_t *router;

   if (!(vm = hypervisor_find_vm(conn,argv[0],VM_TYPE_C3745)))
      return(-1);

   router = VM_C3745(vm);

   nm_bay = atoi(argv[1]);
   port_id = atoi(argv[2]);

   if (c3745_nm_disable_nio(router,nm_bay,port_id) == -1) {
      vm_release(vm);
      hypervisor_send_reply(conn,HSC_ERR_BINDING,1,
                            "C3745 %s: unable to unset NIO for "
                            "interface %u/%u",
                            argv[0],nm_bay,port_id);
      return(-1);
   }

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Show C3745 hardware */
static int cmd_show_hardware(hypervisor_conn_t *conn,int argc,char *argv[])
{
   vm_instance_t *vm;
   c3745_t *router;

   if (!(vm = hypervisor_find_vm(conn,argv[0],VM_TYPE_C3745)))
      return(-1);

   router = VM_C3745(vm);
   c3745_show_hardware(router);

   vm_release(vm);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* Show info about C3745 object */
static void cmd_show_c3745_list(registry_entry_t *entry,void *opt,int *err)
{
   hypervisor_conn_t *conn = opt;
   vm_instance_t *vm = entry->data;

   if (vm->type == VM_TYPE_C3745)
      hypervisor_send_reply(conn,HSC_INFO_MSG,0,"%s",entry->name);
}

/* C3745 List */
static int cmd_c3745_list(hypervisor_conn_t *conn,int argc,char *argv[])
{
   int err = 0;
   registry_foreach_type(OBJ_TYPE_VM,cmd_show_c3745_list,conn,&err);
   hypervisor_send_reply(conn,HSC_INFO_OK,1,"OK");
   return(0);
}

/* C3745 commands */
static hypervisor_cmd_t c3745_cmd_array[] = {
   { "create", 2, 2, cmd_create, NULL },
   { "delete", 1, 1, cmd_delete, NULL },
   { "set_iomem", 2, 2, cmd_set_iomem, NULL },
   { "set_mac_addr", 2, 2, cmd_set_mac_addr, NULL },
   { "start", 1, 1, cmd_start, NULL },
   { "stop", 1, 1, cmd_stop, NULL },
   { "nm_bindings", 1, 1, cmd_nm_bindings, NULL },
   { "nm_nio_bindings", 2, 2, cmd_nm_nio_bindings, NULL },
   { "add_nm_binding", 3, 3, cmd_add_nm_binding, NULL },
   { "remove_nm_binding", 2, 2, cmd_remove_nm_binding, NULL },
   { "add_nio_binding", 4, 4, cmd_add_nio_binding, NULL },
   { "remove_nio_binding", 3, 3, cmd_remove_nio_binding, NULL },
   { "nm_enable_nio", 3, 3, cmd_nm_enable_nio, NULL },
   { "nm_disable_nio", 3, 3, cmd_nm_disable_nio, NULL },
   { "show_hardware", 1, 1, cmd_show_hardware, NULL },
   { "list", 0, 0, cmd_c3745_list, NULL },
   { NULL, -1, -1, NULL, NULL },
};

/* Hypervisor C3745 initialization */
int hypervisor_c3745_init(void)
{
   hypervisor_module_t *module;

   module = hypervisor_register_module("c3745");
   assert(module != NULL);

   hypervisor_register_cmd_array(module,c3745_cmd_array);
   return(0);
}
