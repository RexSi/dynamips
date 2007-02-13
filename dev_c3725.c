/*
 * Cisco 3725 simulation platform.
 * Copyright (c) 2006 Christophe Fillot (cf@utc.fr)
 *
 * Generic Cisco 3725 routines and definitions (EEPROM,...).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <assert.h>

#include "cpu.h"
#include "vm.h"
#include "dynamips.h"
#include "memory.h"
#include "device.h"
#include "pci_io.h"
#include "dev_gt.h"
#include "cisco_eeprom.h"
#include "dev_rom.h"
#include "dev_c3725.h"
#include "dev_vtty.h"
#include "registry.h"

/* ======================================================================== */
/* EEPROM definitions                                                       */
/* ======================================================================== */

/* Cisco 3725 mainboard EEPROM */
static m_uint16_t eeprom_c3725_mainboard_data[] = {
   0x04FF, 0xC18B, 0x5858, 0x5858, 0x5858, 0x5858, 0x5858, 0x5809,
   0x6140, 0x0259, 0xC046, 0x0320, 0x003F, 0x1302, 0x4244, 0x3085,
   0x1C10, 0x8206, 0x80FF, 0xFFFF, 0xFFC4, 0x08FF, 0xFFFF, 0xFFFF,
   0xFFFF, 0xFF81, 0xFFFF, 0xFFFF, 0x03FF, 0x04FF, 0xC28B, 0x5858,
   0x5858, 0x5858, 0x5858, 0x5858, 0x58C3, 0x0600, 0x1319, 0x5C6F,
   0x7043, 0x0030, 0xC508, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x4100,
   0x0101, 0x02FF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
   0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
};

struct cisco_eeprom eeprom_c3725_mainboard = {
   "C3725 Backplane", 
   eeprom_c3725_mainboard_data,
   sizeof(eeprom_c3725_mainboard_data)/2,
};

/* ======================================================================== */
/* Network Module Drivers                                                   */
/* ======================================================================== */
static struct c3725_nm_driver *nm_drivers[] = {
   &dev_c3725_nm_1fe_tx_driver,
   &dev_c3725_nm_16esw_driver,
   &dev_c3725_gt96100_fe_driver,
   &dev_c3725_nm_4t_driver,
   NULL,
};

/* ======================================================================== */
/* Cisco 3725 router instances                                              */
/* ======================================================================== */

/* Directly extract the configuration from the NVRAM device */
ssize_t c3725_nvram_extract_config(vm_instance_t *vm,char **buffer)
{   
   u_char *base_ptr,*ios_ptr,*cfg_ptr,*end_ptr;
   m_uint32_t start,nvlen;
   m_uint16_t magic1,magic2; 
   struct vdevice *nvram_dev;
   off_t nvram_size;
   int fd;

   if ((nvram_dev = dev_get_by_name(vm,"rom")))
      dev_sync(nvram_dev);

   fd = vm_mmap_open_file(vm,"rom",&base_ptr,&nvram_size);

   if (fd == -1)
      return(-1);

   ios_ptr = base_ptr + C3725_NVRAM_OFFSET;
   end_ptr = base_ptr + nvram_size;

   if ((ios_ptr + 0x30) >= end_ptr) {
      vm_error(vm,"NVRAM file too small\n");
      return(-1);
   }

   magic1  = ntohs(*PTR_ADJUST(m_uint16_t *,ios_ptr,0x06));
   magic2  = ntohs(*PTR_ADJUST(m_uint16_t *,ios_ptr,0x08));

   if ((magic1 != 0xF0A5) || (magic2 != 0xABCD)) {
      vm_error(vm,"unable to find IOS magic numbers (0x%x,0x%x)!\n",
               magic1,magic2);
      return(-1);
   }

   start = ntohl(*PTR_ADJUST(m_uint32_t *,ios_ptr,0x10)) + 1;
   nvlen = ntohl(*PTR_ADJUST(m_uint32_t *,ios_ptr,0x18));

   if (!(*buffer = malloc(nvlen+1))) {
      vm_error(vm,"unable to allocate config buffer (%u bytes)\n",nvlen);
      return(-1);
   }

   cfg_ptr = ios_ptr + start + 0x08;

   if ((cfg_ptr + nvlen) > end_ptr) {
      vm_error(vm,"NVRAM file too small\n");
      return(-1);
   }

   memcpy(*buffer,cfg_ptr,nvlen-1);
   (*buffer)[nvlen-1] = 0;
   return(nvlen-1);
}

static int c3725_nvram_push_config_part(vm_instance_t *vm,
                                        char *buffer,size_t len,
                                        u_char *ios_ptr)
{
   m_uint32_t cfg_offset,cklen,tmp;
   m_uint16_t cksum;
   u_char *cfg_ptr;

   cfg_offset = 0x2c;
   cfg_ptr    = ios_ptr + cfg_offset;

   /* Write IOS tag, uncompressed config... */
   *PTR_ADJUST(m_uint16_t *,ios_ptr,0x06) = htons(0xF0A5);
   *PTR_ADJUST(m_uint16_t *,ios_ptr,0x08) = htons(0xABCD);
   *PTR_ADJUST(m_uint16_t *,ios_ptr,0x0a) = htons(0x0001);
   *PTR_ADJUST(m_uint16_t *,ios_ptr,0x0c) = htons(0x0000);
   *PTR_ADJUST(m_uint16_t *,ios_ptr,0x0e) = htons(0x0c04);

   /* Store file contents to NVRAM */
   memcpy(cfg_ptr,buffer,len);

   /* Write config addresses + size */
   tmp = cfg_offset - 0x08;

   *PTR_ADJUST(m_uint32_t *,ios_ptr,0x10) = htonl(tmp);
   *PTR_ADJUST(m_uint32_t *,ios_ptr,0x14) = htonl(tmp + len);
   *PTR_ADJUST(m_uint32_t *,ios_ptr,0x18) = htonl(len);

   /* Compute the checksum */
   cklen = C3725_NVRAM_SIZE - 0x08;
   cksum = nvram_cksum((m_uint16_t *)(ios_ptr+0x08),cklen);
   *PTR_ADJUST(m_uint16_t *,ios_ptr,0x0c) = htons(cksum);
   return(0);
}

/* Directly push the IOS configuration to the NVRAM device */
int c3725_nvram_push_config(vm_instance_t *vm,char *buffer,size_t len)
{
   u_char *base_ptr,*ios_ptr;
   int fd;
   
   fd = vm_mmap_create_file(vm,"rom",vm->rom_size*1048576,&base_ptr);

   if (fd == -1)
      return(-1);

   ios_ptr = base_ptr + C3725_NVRAM_OFFSET;

   /* Normal config */
   c3725_nvram_push_config_part(vm,buffer,len,ios_ptr);
   
   /* Backup config */
   c3725_nvram_push_config_part(vm,buffer,len,ios_ptr + C3725_NVRAM_SIZE);

   vm_mmap_close_file(fd,base_ptr,vm->rom_size*1048576);
   return(0);
}

/* Check for empty config */
int c3725_nvram_check_empty_config(vm_instance_t *vm)
{
   struct vdevice *rom_dev;
   m_uint64_t addr;
   size_t len;

   if (!(rom_dev = dev_get_by_name(vm,"rom")))
      return(-1);

   addr = rom_dev->phys_addr + C3725_NVRAM_OFFSET;
   len  = C3725_NVRAM_SIZE;

   while(len > 0) {
      if (physmem_copy_u32_from_vm(vm,addr) != 0)
         return(0);

      addr += sizeof(m_uint32_t);
      len  -= sizeof(m_uint32_t);
   }

   /* Empty NVRAM */
   vm->conf_reg |= 0x0040;
   printf("NVRAM is empty, setting config register to 0x%x\n",vm->conf_reg);
   return(0);
}

/* Create a new router instance */
c3725_t *c3725_create_instance(char *name,int instance_id)
{
   c3725_t *router;

   if (!(router = malloc(sizeof(*router)))) {
      fprintf(stderr,"C3725 '%s': Unable to create new instance!\n",name);
      return NULL;
   }

   memset(router,0,sizeof(*router));

   if (!(router->vm = vm_create(name,instance_id,VM_TYPE_C3725))) {
      fprintf(stderr,"C3725 '%s': unable to create VM instance!\n",name);
      goto err_vm;
   }

   c3725_init_defaults(router);
   router->vm->hw_data = router;
   return router;

 err_vm:
   free(router);
   return NULL;
}

/* Free resources used by a router instance */
static int c3725_free_instance(void *data,void *arg)
{
   vm_instance_t *vm = data;
   c3725_t *router;
   int i;

   if (vm->type == VM_TYPE_C3725) {
      router = VM_C3725(vm);

      /* Stop all CPUs */
      if (vm->cpu_group != NULL) {
         vm_stop(vm);
      
         if (cpu_group_sync_state(vm->cpu_group) == -1) {
            vm_error(vm,"unable to sync with system CPUs.\n");
            return(FALSE);
         }
      }

      /* Remove NIO bindings */
      for(i=0;i<C3725_MAX_NM_BAYS;i++)
         c3725_nm_remove_all_nio_bindings(router,i);

      /* Shutdown all Network Modules */
      c3725_nm_shutdown_all(router);

      /* Free mainboard EEPROM */
      cisco_eeprom_free(&router->mb_eeprom);

      /* Free all resources used by VM */
      vm_free(vm);

      /* Free the router structure */
      free(router);
      return(TRUE);
   }

   return(FALSE);
}

/* Delete a router instance */
int c3725_delete_instance(char *name)
{
   return(registry_delete_if_unused(name,OBJ_TYPE_VM,
                                    c3725_free_instance,NULL));
}

/* Delete all router instances */
int c3725_delete_all_instances(void)
{
   return(registry_delete_type(OBJ_TYPE_VM,c3725_free_instance,NULL));
}

/* Save configuration of a C3725 instance */
void c3725_save_config(c3725_t *router,FILE *fd)
{
   vm_instance_t *vm = router->vm;
   struct c3725_nio_binding *nb;
   struct c3725_nm_bay *bay;
   int i;

   /* General settings */
   fprintf(fd,"c3725 create %s %u\n",vm->name,vm->instance_id);

   /* VM configuration */
   vm_save_config(vm,fd);

   /* Network Module settings */
   for(i=0;i<C3725_MAX_NM_BAYS;i++) {
      if (!(bay = c3725_nm_get_info(router,i)))
         continue;

      if (bay->dev_type) {
         fprintf(fd,"c3725 add_nm_binding %s %u %s\n",
                 vm->name,i,bay->dev_type);
      }

      for(nb=bay->nio_list;nb;nb=nb->next) {
         fprintf(fd,"c3725 add_nio_binding %s %u %u %s\n",
                 vm->name,i,nb->port_id,nb->nio->name);
      }
   }

   fprintf(fd,"\n");
}

/* Save configurations of all C3725 instances */
static void c3725_reg_save_config(registry_entry_t *entry,void *opt,int *err)
{
   vm_instance_t *vm = entry->data;
   c3725_t *router = VM_C3725(vm);

   if (vm->type == VM_TYPE_C3725)
      c3725_save_config(router,(FILE *)opt);
}

void c3725_save_config_all(FILE *fd)
{
   registry_foreach_type(OBJ_TYPE_VM,c3725_reg_save_config,fd,NULL);
}

/* Get PCI device for the specified NM bay */
int c3725_nm_get_pci_device(u_int nm_bay)
{
   switch(nm_bay) {
      case 1:
         return(0x06);
      case 2:
         return(0x0A);
      default:
         return(-1);
   }
}

/* Set NM EEPROM definition */
int c3725_nm_set_eeprom(c3725_t *router,u_int nm_bay,
                        const struct cisco_eeprom *eeprom)
{
   if (!nm_bay || (nm_bay >= C3725_MAX_NM_BAYS)) {
      vm_error(router->vm,"c3725_nm_set_eeprom: invalid NM Bay %u.\n",nm_bay);
      return(-1);
   }
   
   if (cisco_eeprom_copy(&router->nm_bay[nm_bay].eeprom,eeprom) == -1) {
      vm_error(router->vm,"c3725_nm_set_eeprom: no memory.\n");
      return(-1);
   }
   
   return(0);
}

/* Unset NM EEPROM definition (empty bay) */
int c3725_nm_unset_eeprom(c3725_t *router,u_int nm_bay)
{
   if (!nm_bay || (nm_bay >= C3725_MAX_NM_BAYS)) {
      vm_error(router->vm,"c3725_nm_set_eeprom: invalid NM Bay %u.\n",nm_bay);
      return(-1);
   }
   
   cisco_eeprom_free(&router->nm_bay[nm_bay].eeprom);
   return(0);
}

/* Check if a bay has a port adapter */
int c3725_nm_check_eeprom(c3725_t *router,u_int nm_bay)
{
   if (!nm_bay || (nm_bay >= C3725_MAX_NM_BAYS))
      return(FALSE);

   return(cisco_eeprom_valid(&router->nm_bay[nm_bay].eeprom));
}

/* Get bay info */
struct c3725_nm_bay *c3725_nm_get_info(c3725_t *router,u_int nm_bay)
{
   if (nm_bay >= C3725_MAX_NM_BAYS)
      return NULL;

   return(&router->nm_bay[nm_bay]);
}

/* Get NM type */
char *c3725_nm_get_type(c3725_t *router,u_int nm_bay)
{
   struct c3725_nm_bay *bay;

   bay = c3725_nm_get_info(router,nm_bay);
   return((bay != NULL) ? bay->dev_type : NULL);
}

/* Get driver info about the specified slot */
void *c3725_nm_get_drvinfo(c3725_t *router,u_int nm_bay)
{
   struct c3725_nm_bay *bay;

   bay = c3725_nm_get_info(router,nm_bay);
   return((bay != NULL) ? bay->drv_info : NULL);
}

/* Set driver info for the specified slot */
int c3725_nm_set_drvinfo(c3725_t *router,u_int nm_bay,void *drv_info)
{
   struct c3725_nm_bay *bay;

   if (!(bay = c3725_nm_get_info(router,nm_bay)))
      return(-1);

   bay->drv_info = drv_info;
   return(0);
}

/* Get a NM driver */
static struct c3725_nm_driver *c3725_nm_get_driver(char *dev_type)
{
   int i;

   for(i=0;nm_drivers[i];i++)
      if (!strcmp(nm_drivers[i]->dev_type,dev_type))
         return nm_drivers[i];

   return NULL;
}

/* Add a NM binding */
int c3725_nm_add_binding(c3725_t *router,char *dev_type,u_int nm_bay)
{   
   struct c3725_nm_driver *nm_driver;
   struct c3725_nm_bay *bay;

   if (!(bay = c3725_nm_get_info(router,nm_bay)))
      return(-1);

   /* check that this bay is empty */
   if (bay->dev_type != NULL) {
      vm_error(router->vm,"a NM already exists in slot %u.\n",nm_bay);
      return(-1);
   }

   /* find the NM driver */
   if (!(nm_driver = c3725_nm_get_driver(dev_type))) {
      vm_error(router->vm,"unknown NM type '%s'.\n",dev_type);
      return(-1);
   }

   bay->dev_type = nm_driver->dev_type;
   bay->nm_driver = nm_driver;
   return(0);  
}

/* Remove a NM binding */
int c3725_nm_remove_binding(c3725_t *router,u_int nm_bay)
{   
   struct c3725_nm_bay *bay;

   if (!(bay = c3725_nm_get_info(router,nm_bay)))
      return(-1);

   /* stop if this bay is still active */
   if (bay->drv_info != NULL) {
      vm_error(router->vm,"slot %u still active.\n",nm_bay);
      return(-1);
   }

   /* check that this bay is not empty */
   if (bay->dev_type == NULL) {
      vm_error(router->vm,"slot %u is empty.\n",nm_bay);
      return(-1);
   }
   
   /* remove all NIOs bindings */ 
   c3725_nm_remove_all_nio_bindings(router,nm_bay);

   bay->dev_type  = NULL;
   bay->nm_driver = NULL;
   return(0);
}

/* Find a NIO binding */
struct c3725_nio_binding *
c3725_nm_find_nio_binding(c3725_t *router,u_int nm_bay,u_int port_id)
{   
   struct c3725_nio_binding *nb;
   struct c3725_nm_bay *bay;

   if (!(bay = c3725_nm_get_info(router,nm_bay)))
      return NULL;

   for(nb=bay->nio_list;nb;nb=nb->next)
      if (nb->port_id == port_id)
         return nb;

   return NULL;
}

/* Add a network IO binding */
int c3725_nm_add_nio_binding(c3725_t *router,u_int nm_bay,u_int port_id,
                             char *nio_name)
{
   struct c3725_nio_binding *nb;
   struct c3725_nm_bay *bay;
   netio_desc_t *nio;

   if (!(bay = c3725_nm_get_info(router,nm_bay)))
      return(-1);

   /* check that a NIO is not already bound to this port */
   if (c3725_nm_find_nio_binding(router,nm_bay,port_id) != NULL) {
      vm_error(router->vm,"a NIO already exists for interface %u/%u.\n",
               nm_bay,port_id);
      return(-1);
   }

   /* acquire a reference on the NIO object */
   if (!(nio = netio_acquire(nio_name))) {
      vm_error(router->vm,"unable to find NIO '%s'.\n",nio_name);
      return(-1);
   }

   /* create a new binding */
   if (!(nb = malloc(sizeof(*nb)))) {
      vm_error(router->vm,"unable to create NIO binding "
               "for interface %u/%u.\n",nm_bay,port_id);
      netio_release(nio_name);
      return(-1);
   }

   memset(nb,0,sizeof(*nb));
   nb->nio       = nio;
   nb->port_id   = port_id;
   nb->next      = bay->nio_list;
   if (nb->next) nb->next->prev = nb;
   bay->nio_list = nb;
   return(0);
}

/* Remove a NIO binding */
int c3725_nm_remove_nio_binding(c3725_t *router,u_int nm_bay,u_int port_id)
{
   struct c3725_nio_binding *nb;
   struct c3725_nm_bay *bay;
   
   if (!(bay = c3725_nm_get_info(router,nm_bay)))
      return(-1);

   if (!(nb = c3725_nm_find_nio_binding(router,nm_bay,port_id)))
      return(-1);   /* no nio binding for this slot/port */

   /* tell the NM driver to stop using this NIO */
   if (bay->nm_driver)
      bay->nm_driver->nm_unset_nio(router,nm_bay,port_id);

   /* remove this entry from the double linked list */
   if (nb->next)
      nb->next->prev = nb->prev;

   if (nb->prev) {
      nb->prev->next = nb->next;
   } else {
      bay->nio_list = nb->next;
   }

   /* unreference NIO object */
   netio_release(nb->nio->name);
   free(nb);
   return(0);
}

/* Remove all NIO bindings for the specified NM */
int c3725_nm_remove_all_nio_bindings(c3725_t *router,u_int nm_bay)
{  
   struct c3725_nio_binding *nb,*next;
   struct c3725_nm_bay *bay;

   if (!(bay = c3725_nm_get_info(router,nm_bay)))
      return(-1);

   for(nb=bay->nio_list;nb;nb=next) {
      next = nb->next;

      /* tell the NM driver to stop using this NIO */
      if (bay->nm_driver)
         bay->nm_driver->nm_unset_nio(router,nm_bay,nb->port_id);

      /* unreference NIO object */
      netio_release(nb->nio->name);
      free(nb);
   }

   bay->nio_list = NULL;
   return(0);
}

/* Enable a Network IO descriptor for a Network Module */
int c3725_nm_enable_nio(c3725_t *router,u_int nm_bay,u_int port_id)
{
   struct c3725_nio_binding *nb;
   struct c3725_nm_bay *bay;

   if (!(bay = c3725_nm_get_info(router,nm_bay)))
      return(-1);

   /* check that we have an NIO binding for this interface */
   if (!(nb = c3725_nm_find_nio_binding(router,nm_bay,port_id)))
      return(-1);

   /* check that the driver is defined and successfully initialized */
   if (!bay->nm_driver || !bay->drv_info)
      return(-1);

   return(bay->nm_driver->nm_set_nio(router,nm_bay,port_id,nb->nio));
}

/* Disable Network IO descriptor of a Network Module */
int c3725_nm_disable_nio(c3725_t *router,u_int nm_bay,u_int port_id)
{
   struct c3725_nm_bay *bay;

   if (!(bay = c3725_nm_get_info(router,nm_bay)))
      return(-1);

   /* check that the driver is defined and successfully initialized */
   if (!bay->nm_driver || !bay->drv_info)
      return(-1);

   return(bay->nm_driver->nm_unset_nio(router,nm_bay,port_id));
}

/* Enable all NIO of the specified NM */
int c3725_nm_enable_all_nio(c3725_t *router,u_int nm_bay)
{
   struct c3725_nio_binding *nb;
   struct c3725_nm_bay *bay;

   if (!(bay = c3725_nm_get_info(router,nm_bay)))
      return(-1);

   /* check that the driver is defined and successfully initialized */
   if (!bay->nm_driver || !bay->drv_info)
      return(-1);

   for(nb=bay->nio_list;nb;nb=nb->next)
      bay->nm_driver->nm_set_nio(router,nm_bay,nb->port_id,nb->nio);

   return(0);
}

/* Disable all NIO of the specified NM */
int c3725_nm_disable_all_nio(c3725_t *router,u_int nm_bay)
{
   struct c3725_nio_binding *nb;
   struct c3725_nm_bay *bay;

   if (!(bay = c3725_nm_get_info(router,nm_bay)))
      return(-1);

   /* check that the driver is defined and successfully initialized */
   if (!bay->nm_driver || !bay->drv_info)
      return(-1);

   for(nb=bay->nio_list;nb;nb=nb->next)
      bay->nm_driver->nm_unset_nio(router,nm_bay,nb->port_id);

   return(0);
}

/* Initialize a Network Module */
int c3725_nm_init(c3725_t *router,u_int nm_bay)
{   
   struct c3725_nm_bay *bay;
   size_t len;

   if (!(bay = c3725_nm_get_info(router,nm_bay)))
      return(-1);

   /* Check that a device type is defined for this bay */
   if (!bay->dev_type || !bay->nm_driver) {
      vm_error(router->vm,"trying to init empty slot %u.\n",nm_bay);
      return(-1);
   }

   /* Allocate device name */
   len = strlen(bay->dev_type) + 10;
   if (!(bay->dev_name = malloc(len))) {
      vm_error(router->vm,"unable to allocate device name.\n");
      return(-1);
   }

   snprintf(bay->dev_name,len,"%s(%u)",bay->dev_type,nm_bay);

   /* Initialize NM driver */
   if (bay->nm_driver->nm_init(router,bay->dev_name,nm_bay) == 1) {
      vm_error(router->vm,"unable to initialize NM %u.\n",nm_bay);
      return(-1);
   }

   /* Enable all NIO */
   c3725_nm_enable_all_nio(router,nm_bay);
   return(0);
}

/* Shutdown a Network Module */
int c3725_nm_shutdown(c3725_t *router,u_int nm_bay)
{
   struct c3725_nm_bay *bay;

   if (!(bay = c3725_nm_get_info(router,nm_bay)))
      return(-1);

   /* Check that a device type is defined for this bay */   
   if (!bay->dev_type || !bay->nm_driver) {
      vm_error(router->vm,"trying to shut down empty slot %u.\n",nm_bay);
      return(-1);
   }

   /* Disable all NIO */
   c3725_nm_disable_all_nio(router,nm_bay);

   /* Shutdown the NM driver */
   if (bay->drv_info && (bay->nm_driver->nm_shutdown(router,nm_bay) == -1)) {
      vm_error(router->vm,"unable to shutdown NM %u.\n",nm_bay);
      return(-1);
   }

   free(bay->dev_name);
   bay->dev_name = NULL;
   bay->drv_info = NULL;
   return(0);
}

/* Shutdown all NM of a router */
int c3725_nm_shutdown_all(c3725_t *router)
{
   int i;

   for(i=0;i<C3725_MAX_NM_BAYS;i++) {
      if (!router->nm_bay[i].dev_type) 
         continue;

      c3725_nm_shutdown(router,i);
   }

   return(0);
}

/* Show info about all NMs */
int c3725_nm_show_all_info(c3725_t *router)
{
   struct c3725_nm_bay *bay;
   int i;

   for(i=0;i<C3725_MAX_NM_BAYS;i++) {
      if (!(bay = c3725_nm_get_info(router,i)) || !bay->nm_driver)
         continue;

      if (bay->nm_driver->nm_show_info != NULL)
         bay->nm_driver->nm_show_info(router,i);
   }

   return(0);
}

/* Maximum number of tokens in a NM description */
#define NM_DESC_MAX_TOKENS  8

/* Create a Network Module (command line) */
int c3725_cmd_nm_create(c3725_t *router,char *str)
{
   char *tokens[NM_DESC_MAX_TOKENS];
   int i,count,res;
   u_int nm_bay;

   /* A port adapter description is like "1:NM-1FE" */
   if ((count = m_strsplit(str,':',tokens,NM_DESC_MAX_TOKENS)) != 2) {
      vm_error(router->vm,"unable to parse NM description '%s'.\n",str);
      return(-1);
   }

   /* Parse the NM bay id */
   nm_bay = atoi(tokens[0]);

   /* Add this new NM to the current NM list */
   res = c3725_nm_add_binding(router,tokens[1],nm_bay);

   /* The complete array was cleaned by strsplit */
   for(i=0;i<NM_DESC_MAX_TOKENS;i++)
      free(tokens[i]);

   return(res);
}

/* Add a Network IO descriptor binding (command line) */
int c3725_cmd_add_nio(c3725_t *router,char *str)
{
   char *tokens[NM_DESC_MAX_TOKENS];
   int i,count,nio_type,res=-1;
   u_int nm_bay,port_id;
   netio_desc_t *nio;
   char nio_name[128];

   /* A port adapter description is like "1:3:tap:tap0" */
   if ((count = m_strsplit(str,':',tokens,NM_DESC_MAX_TOKENS)) < 3) {
      vm_error(router->vm,"unable to parse NIO description '%s'.\n",str);
      return(-1);
   }

   /* Parse the NM bay */
   nm_bay = atoi(tokens[0]);

   /* Parse the NM port id */
   port_id = atoi(tokens[1]);

   /* Autogenerate a NIO name */
   snprintf(nio_name,sizeof(nio_name),"c3725-i%u/%u/%u",
            router->vm->instance_id,nm_bay,port_id);

   /* Create the Network IO descriptor */
   nio = NULL;
   nio_type = netio_get_type(tokens[2]);

   switch(nio_type) {
      case NETIO_TYPE_UNIX:
         if (count != 5) {
            vm_error(router->vm,
                     "invalid number of arguments for UNIX NIO '%s'\n",str);
            goto done;
         }

         nio = netio_desc_create_unix(nio_name,tokens[3],tokens[4]);
         break;

      case NETIO_TYPE_VDE:
         if (count != 5) {
            vm_error(router->vm,
                     "invalid number of arguments for VDE NIO '%s'\n",str);
            goto done;
         }

         nio = netio_desc_create_vde(nio_name,tokens[3],tokens[4]);
         break;

      case NETIO_TYPE_TAP:
         if (count != 4) {
            vm_error(router->vm,
                     "invalid number of arguments for TAP NIO '%s'\n",str);
            goto done;
         }

         nio = netio_desc_create_tap(nio_name,tokens[3]);
         break;

      case NETIO_TYPE_UDP:
         if (count != 6) {
            vm_error(router->vm,
                     "invalid number of arguments for UDP NIO '%s'\n",str);
            goto done;
         }

         nio = netio_desc_create_udp(nio_name,atoi(tokens[3]),
                                     tokens[4],atoi(tokens[5]));
         break;

      case NETIO_TYPE_TCP_CLI:
         if (count != 5) {
            vm_error(router->vm,
                     "invalid number of arguments for TCP CLI NIO '%s'\n",str);
            goto done;
         }

         nio = netio_desc_create_tcp_cli(nio_name,tokens[3],tokens[4]);
         break;

      case NETIO_TYPE_TCP_SER:
         if (count != 4) {
            vm_error(router->vm,
                     "invalid number of arguments for TCP SER NIO '%s'\n",str);
            goto done;
         }

         nio = netio_desc_create_tcp_ser(nio_name,tokens[3]);
         break;

      case NETIO_TYPE_NULL:
         nio = netio_desc_create_null(nio_name);
         break;

#ifdef LINUX_ETH
      case NETIO_TYPE_LINUX_ETH:
         if (count != 4) {
            vm_error(router->vm,
                     "invalid number of arguments for Linux Eth NIO '%s'\n",
                     str);
            goto done;
         }
         
         nio = netio_desc_create_lnxeth(nio_name,tokens[3]);
         break;
#endif

#ifdef GEN_ETH
      case NETIO_TYPE_GEN_ETH:
         if (count != 4) {
            vm_error(router->vm,
                     "invalid number of arguments for Generic Eth NIO '%s'\n",
                     str);
            goto done;
         }
         
         nio = netio_desc_create_geneth(nio_name,tokens[3]);
         break;
#endif

      default:
         vm_error(router->vm,"unknown NETIO type '%s'\n",tokens[2]);
         goto done;
   }

   if (!nio) {
      vm_error(router->vm,"unable to create NETIO "
              "descriptor for NM slot %u\n",nm_bay);
      goto done;
   }

   if (c3725_nm_add_nio_binding(router,nm_bay,port_id,nio_name) == -1) {
      vm_error(router->vm,"unable to add NETIO binding for slot %u\n",nm_bay);
      netio_release(nio_name);
      netio_delete(nio_name);
      goto done;
   }
   
   netio_release(nio_name);
   res = 0;

 done:
   /* The complete array was cleaned by strsplit */
   for(i=0;i<NM_DESC_MAX_TOKENS;i++)
      free(tokens[i]);

   return(res);
}

/* Show the list of available NM drivers */
void c3725_nm_show_drivers(void)
{
   int i;

   printf("Available C3725 Network Module drivers:\n");

   for(i=0;nm_drivers[i];i++) {
      printf("  * %s %s\n",
             nm_drivers[i]->dev_type,
             !nm_drivers[i]->supported ? "(NOT WORKING)" : "");
   }
   
   printf("\n");
}

/* Set the base MAC address of the chassis */
static int c3725_burn_mac_addr(c3725_t *router,n_eth_addr_t *addr)
{
   m_uint8_t eeprom_ver;
   size_t offset;

   /* Read EEPROM format version */
   cisco_eeprom_get_byte(&router->mb_eeprom,0,&eeprom_ver);

   switch(eeprom_ver) {
      case 0:
         cisco_eeprom_set_region(&router->mb_eeprom,2,addr->eth_addr_byte,6);
         break;

      case 4:
         if (!cisco_eeprom_v4_find_field(&router->mb_eeprom,0xC3,&offset)) {
            cisco_eeprom_set_region(&router->mb_eeprom,offset,
                                    addr->eth_addr_byte,6);
         }
         break;

      default:
         vm_error(router->vm,"c3725_burn_mac_addr: unable to handle "
                  "EEPROM version %u\n",eeprom_ver);
         return(-1);
   }

   return(0);
}

/* Set chassis MAC address */
int c3725_chassis_set_mac_addr(c3725_t *router,char *mac_addr)
{
   if (parse_mac_addr(&router->mac_addr,mac_addr) == -1) {
      vm_error(router->vm,"unable to parse MAC address '%s'.\n",mac_addr);
      return(-1);
   }

   /* Set the chassis base MAC address */
   c3725_burn_mac_addr(router,&router->mac_addr);
   return(0);
}

/* Create the two main PCI busses for a GT64120 based system */
static int c3725_init_gt96100(c3725_t *router)
{
   vm_instance_t *vm = router->vm;

   vm->pci_bus[0] = pci_bus_create("PCI bus #0",0);
   vm->pci_bus[1] = pci_bus_create("PCI bus #1",0);

   if (!vm->pci_bus[0] || !vm->pci_bus[1]) {
      vm_error(router->vm,"unable to create PCI data.\n");
      return(-1);
   }
   
   return(dev_gt96100_init(vm,"gt96100",C3725_GT96K_ADDR,0x200000,
                           C3725_GT96K_IRQ,C3725_NETIO_IRQ));
}

/* Initialize a Cisco 3725 */
static int c3725_init(c3725_t *router)
{   
   vm_instance_t *vm = router->vm;

   /* Set the processor type: R7000 */
   mips64_set_prid(CPU_MIPS64(vm->boot_cpu),MIPS_PRID_R7000);

   /* Initialize the Galileo GT-96100 PCI controller */
   if (c3725_init_gt96100(router) == -1)
      return(-1);

   /* Initialize PCI map (NM slot 1 & 2) */
   router->nm_bay[1].pci_map = vm->pci_bus[1];
   router->nm_bay[2].pci_map = vm->pci_bus[1];

   vm->elf_machine_id = C3725_ELF_MACHINE_ID;
   return(0);
}

/* Show C3725 hardware info */
void c3725_show_hardware(c3725_t *router)
{
   vm_instance_t *vm = router->vm;

   printf("C3725 instance '%s' (id %d):\n",vm->name,vm->instance_id);

   printf("  VM Status  : %d\n",vm->status);
   printf("  RAM size   : %u Mb\n",vm->ram_size);
   printf("  NVRAM size : %u Kb\n",vm->nvram_size);
   printf("  IOS image  : %s\n\n",vm->ios_image);

   if (vm->debug_level > 0) {
      dev_show_list(vm);
      pci_dev_show_list(vm->pci_bus[0]);
      pci_dev_show_list(vm->pci_bus[1]);
      printf("\n");
   }
}

/* Initialize default parameters for a C3725 */
void c3725_init_defaults(c3725_t *router)
{   
   vm_instance_t *vm = router->vm;   
   n_eth_addr_t *m;
   m_uint16_t pid;

   pid = (m_uint16_t)getpid();

   /* Generate a chassis MAC address based on the instance ID */
   m = &router->mac_addr;
   m->eth_addr_byte[0] = vm_get_mac_addr_msb(vm);
   m->eth_addr_byte[1] = vm->instance_id & 0xFF;
   m->eth_addr_byte[2] = pid >> 8;
   m->eth_addr_byte[3] = pid & 0xFF;
   m->eth_addr_byte[4] = 0x00;
   m->eth_addr_byte[5] = 0x00;

   c3725_init_eeprom_groups(router);
   cisco_eeprom_copy(&router->mb_eeprom,&eeprom_c3725_mainboard);
   c3725_burn_mac_addr(router,&router->mac_addr);

   vm->ram_mmap          = C3725_DEFAULT_RAM_MMAP;
   vm->ram_size          = C3725_DEFAULT_RAM_SIZE;
   vm->rom_size          = C3725_DEFAULT_ROM_SIZE;
   vm->nvram_size        = C3725_DEFAULT_NVRAM_SIZE;
   vm->conf_reg_setup    = C3725_DEFAULT_CONF_REG;
   vm->clock_divisor     = C3725_DEFAULT_CLOCK_DIV;
   vm->nvram_rom_space   = C3725_NVRAM_ROM_RES_SIZE;
   router->nm_iomem_size = C3725_DEFAULT_IOMEM_SIZE;

   vm->pcmcia_disk_size[0] = C3725_DEFAULT_DISK0_SIZE;
   vm->pcmcia_disk_size[1] = C3725_DEFAULT_DISK1_SIZE;

   /* Enable NVRAM operations to load/store configs */
   vm->nvram_extract_config = c3725_nvram_extract_config;
   vm->nvram_push_config = c3725_nvram_push_config;
}

/* Initialize the C3725 Platform */
int c3725_init_platform(c3725_t *router)
{
   vm_instance_t *vm = router->vm;
   struct c3725_nm_bay *nm_bay;
   cpu_mips_t *cpu;
   cpu_gen_t *gen;
   vm_obj_t *obj;
   int i;

   /* Copy config register setup into "active" config register */
   vm->conf_reg = vm->conf_reg_setup;

   /* Create Console and AUX ports */
   vm_init_vtty(vm);

   /* Create a CPU group */
   vm->cpu_group = cpu_group_create("System CPU");

   /* Initialize the virtual MIPS processor */
   if (!(gen = cpu_create(vm,CPU_TYPE_MIPS64,0))) {
      vm_error(vm,"unable to create CPU!\n");
      return(-1);
   }

   cpu = CPU_MIPS64(gen);

   /* Add this CPU to the system CPU group */
   cpu_group_add(vm->cpu_group,gen);
   vm->boot_cpu = gen;

   /* Initialize the IRQ routing vectors */
   vm->set_irq = mips64_vm_set_irq;
   vm->clear_irq = mips64_vm_clear_irq;

   /* Mark the Network IO interrupt as high priority */
   cpu->irq_idle_preempt[C3725_NETIO_IRQ] = TRUE;
   cpu->irq_idle_preempt[C3725_GT96K_IRQ] = TRUE;
   cpu->irq_idle_preempt[C3725_DUART_IRQ] = TRUE;

   /* Copy some parameters from VM to CPU (idle PC, ...) */
   cpu->idle_pc = vm->idle_pc;

   if (vm->timer_irq_check_itv)
      cpu->timer_irq_check_itv = vm->timer_irq_check_itv;

   /* Remote emulator control */
   dev_remote_control_init(vm,0x16000000,0x1000);

   /* Specific Storage Area (SSA) */
   dev_ram_init(vm,"ssa",TRUE,FALSE,NULL,FALSE,0x16001000ULL,0x7000);

   /* IO FPGA */
   if (dev_c3725_iofpga_init(router,C3725_IOFPGA_ADDR,0x40000) == -1)
      return(-1);

#if 0
   /* PCI IO space */
   if (!(vm->pci_io_space = pci_io_data_init(vm,C3725_PCI_IO_ADDR)))
      return(-1);
#endif

   /* Initialize the chassis */
   if (c3725_init(router) == -1)
      return(-1);

   /* Initialize RAM */
   vm_ram_init(vm,0x00000000ULL);

   /* Initialize ROM (as a Flash) */
   if (!(obj = dev_flash_init(vm,"rom",C3725_ROM_ADDR,vm->rom_size*1048576)))
      return(-1);

   dev_flash_copy_data(obj,0,mips64_microcode,mips64_microcode_len);
   c3725_nvram_check_empty_config(vm);

   /* Initialize the NS16552 DUART */
   dev_ns16552_init(vm,C3725_DUART_ADDR,0x1000,3,C3725_DUART_IRQ,
                    vm->vtty_con,vm->vtty_aux);

   /* PCMCIA Slot 0 */
   dev_pcmcia_disk_init(vm,"slot0",C3725_SLOT0_ADDR,0x200000,
                        vm->pcmcia_disk_size[0],1);

   /* PCMCIA Slot 1 */
   dev_pcmcia_disk_init(vm,"slot1",C3725_SLOT1_ADDR,0x200000,
                        vm->pcmcia_disk_size[1],1);

   /* The GT96100 system controller has 2 integrated FastEthernet ports */
   c3725_nm_add_binding(router,"GT96100-FE",0);

   /* Initialize Network Modules */
   for(i=0;i<C3725_MAX_NM_BAYS;i++) {
      nm_bay = &router->nm_bay[i];

      if (!nm_bay->dev_type) 
         continue;

      if (c3725_nm_init(router,i) == -1) {
         vm_error(vm,"unable to create Network Module \"%s\"\n",
                  nm_bay->dev_type);
         return(-1);
      }
   }

   /* Show device list */
   c3725_show_hardware(router);
   return(0);
}

/* Boot the IOS image */
int c3725_boot_ios(c3725_t *router)
{   
   vm_instance_t *vm = router->vm;
   cpu_mips_t *cpu;

   if (!vm->boot_cpu)
      return(-1);

   /* Suspend CPU activity since we will restart directly from ROM */
   vm_suspend(vm);

   /* Check that CPU activity is really suspended */
   if (cpu_group_sync_state(vm->cpu_group) == -1) {
      vm_error(vm,"unable to sync with system CPUs.\n");
      return(-1);
   }

   /* Reset the boot CPU */
   cpu = CPU_MIPS64(vm->boot_cpu);
   mips64_reset(cpu);

   /* Load IOS image */
   if (mips64_load_elf_image(cpu,vm->ios_image,
                             (vm->ghost_status == VM_GHOST_RAM_USE),
                             &vm->ios_entry_point) < 0) 
   {
      vm_error(vm,"failed to load Cisco IOS image '%s'.\n",vm->ios_image);
      return(-1);
   }

   /* Launch the simulation */
   printf("\nC3725 '%s': starting simulation (CPU0 PC=0x%llx), "
          "JIT %sabled.\n",
          vm->name,cpu->pc,vm->jit_use ? "en":"dis");

   vm_log(vm,"C3725_BOOT",
          "starting instance (CPU0 PC=0x%llx,idle_pc=0x%llx,JIT %s)\n",
          cpu->pc,cpu->idle_pc,vm->jit_use ? "on":"off");

   /* Start main CPU */
   if (vm->ghost_status != VM_GHOST_RAM_GENERATE) {
      vm->status = VM_STATUS_RUNNING;
      cpu_start(vm->boot_cpu);
   } else {
      vm->status = VM_STATUS_SHUTDOWN;
   }
   return(0);
}

/* Initialize a Cisco 3725 instance */
int c3725_init_instance(c3725_t *router)
{   
   vm_instance_t *vm = router->vm;
   m_uint32_t rom_entry_point;
   cpu_mips_t *cpu0;

   if (!vm->ios_image) {
      vm_error(vm,"no Cisco IOS image defined.");
      return(-1);
   }

   /* Initialize the C3725 platform */
   if (c3725_init_platform(router) == -1) {
      vm_error(vm,"unable to initialize the platform hardware.\n");
      return(-1);
   }

   /* Load IOS configuration file */
   if (vm->ios_config != NULL) {
      vm_nvram_push_config(vm,vm->ios_config);
      vm->conf_reg &= ~0x40;
   }

   /* Load ROM (ELF image or embedded) */
   cpu0 = CPU_MIPS64(vm->boot_cpu);
   rom_entry_point = (m_uint32_t)MIPS_ROM_PC;

   if ((vm->rom_filename != NULL) &&
       (mips64_load_elf_image(cpu0,vm->rom_filename,0,&rom_entry_point) < 0))
   {
      vm_error(vm,"unable to load alternate ROM '%s', "
               "fallback to embedded ROM.\n\n",vm->rom_filename);
      vm->rom_filename = NULL;
   }

   /* Load symbol file */
   if (vm->sym_filename) {
      mips64_sym_load_file(cpu0,vm->sym_filename);
      cpu0->sym_trace = 1;
   }

   return(c3725_boot_ios(router));
}

/* Stop a Cisco 3725 instance */
int c3725_stop_instance(c3725_t *router)
{
   vm_instance_t *vm = router->vm;

   printf("\nC3725 '%s': stopping simulation.\n",vm->name);
   vm_log(vm,"C3725_STOP","stopping simulation.\n");

   /* Stop all CPUs */
   if (vm->cpu_group != NULL) {
      vm_stop(vm);
      
      if (cpu_group_sync_state(vm->cpu_group) == -1) {
         vm_error(vm,"unable to sync with system CPUs.\n");
         return(-1);
      }
   }

   /* Free resources that were used during execution to emulate hardware */
   c3725_nm_shutdown_all(router);
   vm_hardware_shutdown(vm);
   return(0);
}
