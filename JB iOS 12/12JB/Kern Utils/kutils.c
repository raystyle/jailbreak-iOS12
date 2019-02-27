#include <stdio.h>
#include <stdlib.h>

#include <mach/mach.h>
#include "kutils.h"
#include "kmem.h"
#include "offsets.h"
#include "patchfinder64.h"
#include "exploit_additions.h"
#include "offsetof.h"
#include "kernel_slide.h"

extern mach_port_t tfpzero;

uint64_t cached_task_self_addr = 0;
uint64_t task_self_addr() {
    if (cached_task_self_addr == 0) {
        cached_task_self_addr = find_port(mach_task_self());
        printf("task self: 0x%llx\n", cached_task_self_addr);
    }
    return cached_task_self_addr;
}

uint64_t ipc_space_kernel() {
    return rk64(task_self_addr() + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_RECEIVER));
}

uint64_t get_kaslr_slide() {
    if (!kernel_slide_init())
        return 0;
    return kernel_slide;
}

mach_port_t fake_host_priv_port = MACH_PORT_NULL;

// build a fake host priv port
mach_port_t fake_host_priv() {
    if (fake_host_priv_port != MACH_PORT_NULL) {
        return fake_host_priv_port;
    }
    // get the address of realhost:
    uint64_t hostport_addr = find_port(mach_host_self());
    uint64_t realhost = rk64(hostport_addr + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_KOBJECT));
    
    // allocate a port
    mach_port_t port = MACH_PORT_NULL;
    kern_return_t err;
    err = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
    if (err != KERN_SUCCESS) {
        printf("failed to allocate port\n");
        return MACH_PORT_NULL;
    }
    
    // get a send right
    mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
    
    // locate the port
    uint64_t port_addr = find_port(port);
    
    // change the type of the port
#define IKOT_HOST_PRIV 4
#define IO_ACTIVE   0x80000000
    wk32(port_addr + koffset(KSTRUCT_OFFSET_IPC_PORT_IO_BITS), IO_ACTIVE|IKOT_HOST_PRIV);
    
    // change the space of the port
    wk64(port_addr + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_RECEIVER), ipc_space_kernel());
    
    // set the kobject
    wk64(port_addr + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_KOBJECT), realhost);
    
    fake_host_priv_port = port;
    
    return port;
}

size_t kread(uint64_t where, void *p, size_t size) {
    int rv;
    size_t offset = 0;
    while (offset < size) {
        mach_vm_size_t sz, chunk = 2048;
        if (chunk > size - offset) {
            chunk = size - offset;
        }
        rv = mach_vm_read_overwrite(tfpzero, where + offset, chunk, (mach_vm_address_t)p + offset, &sz);
        if (rv || sz == 0) {
            fprintf(stderr, "[e] error reading kernel @%p\n", (void *)(offset + where));
            return -2;
            break;
        }
        offset += sz;
    }
    return offset;
}

size_t kwrite(uint64_t where, const void *p, size_t size) {
    int rv;
    size_t offset = 0;
    while (offset < size) {
        size_t chunk = 2048;
        if (chunk > size - offset) {
            chunk = size - offset;
        }
        rv = mach_vm_write(tfpzero, where + offset, (mach_vm_offset_t)p + offset, (mach_msg_type_number_t)chunk);
        if (rv) {
            fprintf(stderr, "[e] error writing kernel @%p\n", (void *)(offset + where));
            break;
        }
        offset += chunk;
    }
    return offset;
}

uint64_t kalloc(vm_size_t size) {
    mach_vm_address_t address = 0;
    mach_vm_allocate(tfpzero, (mach_vm_address_t *)&address, size, VM_FLAGS_ANYWHERE);
    return address;
}

uint64_t kalloc_wired(uint64_t size) {
    kern_return_t err;
    mach_vm_address_t addr = 0;
    mach_vm_size_t ksize = round_page_kernel(size);
    
    printf("vm_kernel_page_size: %lx\n", vm_kernel_page_size);
    
    err = mach_vm_allocate(tfpzero, &addr, ksize+0x4000, VM_FLAGS_ANYWHERE);
    if (err != KERN_SUCCESS) {
        printf("unable to allocate kernel memory via tfp0: %s %x\n", mach_error_string(err), err);
        sleep(3);
        return 0;
    }
    
    printf("allocated address: %llx\n", addr);
    
    addr += 0x3fff;
    addr &= ~0x3fffull;
    
    printf("address to wire: %llx\n", addr);
    
    err = mach_vm_wire(fake_host_priv(), tfpzero, addr, ksize, VM_PROT_READ|VM_PROT_WRITE);
    if (err != KERN_SUCCESS) {
        printf("unable to wire kernel memory via tfp0: %s %x\n", mach_error_string(err), err);
        sleep(3);
        return 0;
    }
    return addr;
}

void kfree(mach_vm_address_t address, vm_size_t size){
    mach_vm_deallocate(tfpzero, address, size);
}

uint32_t find_pid_of_proc(const char *proc_name) {
    uint64_t proc = rk64(find_allproc());
    while (proc) {
        uint32_t pid = (uint32_t)rk32(proc + offsetof_p_pid);
        char name[40] = {0};
        kread(proc+0x268, name, 20);
        if (strstr(name, proc_name)){
            return pid;
        }
        proc = rk64(proc);
    }
    return 0;
}

uint64_t get_proc_struct_for_pid(pid_t proc_pid) {
    uint64_t proc = rk64(find_allproc());
    while (proc) {
        uint32_t pid = (uint32_t)rk32(proc + offsetof_p_pid);
        if (pid == proc_pid){
            return proc;
        }
        proc = rk64(proc);
    }
    return 0;
}
