/*******************************************************************************
 * Copyright (c) 2015 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *******************************************************************************/
#include <string.h>
#include <assert.h>
#include <arch/x86/mmu.h>
#include <arch/x86.h>
#include <arch/local_apic.h>
#include <kernel/vm.h>
#include <platform/sand.h>
#include <platform/vmcall.h>
#ifdef SPI_CONTROLLER
#include <platform/lpss_spi.h>
#endif

#define GET_STEPPING_ID(val)    ((val) & 0xF)
#define GET_MODEL(val)          (((val) >> 4) & 0xF)
#define GET_FAMILY_ID(val)      (((val) >> 8) & 0xF)
#define SEP_BIT                 11

extern int _start;
extern int _end;
extern uint64_t __code_start;
extern uint64_t __code_end;
extern uint64_t __rodata_start;
extern uint64_t __rodata_end;
extern uint64_t __data_start;
extern uint64_t __data_end;
extern uint64_t __bss_start;
extern uint64_t __bss_end;
extern void arch_mmu_init_percpu(void);
#if PRINT_USE_MMIO
extern void init_uart(void);
#endif

/* Store physical address LK is located. */
uintptr_t entry_phys = 0;

/* For 16MB memory mapping */
map_addr_t pde_kernel[NO_OF_PT_ENTRIES] __ALIGNED(PAGE_SIZE);
/* Acutally needs 8 entries only, 1 more for unalignment mapping */
map_addr_t pte_kernel[NO_OF_PT_ENTRIES * 9] __ALIGNED(PAGE_SIZE);

/* a big pile of page tables needed to map 512GB of memory into kernel space using 2MB pages */
map_addr_t linear_map_pdp_512[(512ULL*GB) / (2*MB)] __ALIGNED(PAGE_SIZE);

device_sec_info_t *g_sec_info = NULL;

enum {
    VMM_ID_EVMM = 0,
    VMM_ID_ACRN,
    VMM_SUPPORTED_NUM
} vmm_id_t;

static const char *vmm_signature[] = {
    [VMM_ID_EVMM] = "EVMMEVMMEVMM",
    [VMM_ID_ACRN] = "ACRNACRNACRN"
};

static inline int detect_vmm(void)
{
    uint32_t signature[3];
    int i;

    __asm__ __volatile__ (
        "cpuid\n\t"
        : "=b" (signature[0]),
          "=c" (signature[1]),
          "=d" (signature[2])
        : "a" (0x40000000)
        : "cc");

    for (i=0; i<VMM_SUPPORTED_NUM; i++) {
        if (!memcmp(vmm_signature[i], signature, 12))
            return i;
    }
    return -1;
}

#ifdef WITH_KERNEL_VM
struct mmu_initial_mapping mmu_initial_mappings[] = {
    /* 16MB of memory mapped where the kernel lives */
    {
        .phys = MEMBASE + KERNEL_LOAD_OFFSET,
        .virt = KERNEL_BASE + KERNEL_LOAD_OFFSET,
        .size = 16*MB,
        .flags = MMU_INITIAL_MAPPING_TEMPORARY,
        .name = "kernel"
    },
    /* 16MB for symbols and PA/VA translation in kernel */
    {
        .phys = 0,
        .virt = KERNEL_ASPACE_BASE,
        .size = 16*MB,
        .flags = 0,
        .name = "krnl_mem"
    },
    {0}
};

static pmm_arena_t heap_arena = {
    .name = "memory",
    .base = MEMBASE,
    .size = 0, /* default amount of memory in case we don't have multiboot */
    .priority = 1,
    .flags = PMM_ARENA_FLAG_KMAP
};

static void heap_arena_init(void)
{
    uint64_t rsvd = (uint64_t)&__bss_end - (uint64_t)(mmu_initial_mappings[0].virt);
    rsvd += KERNEL_LOAD_OFFSET;

    heap_arena.base = PAGE_ALIGN(mmu_initial_mappings[0].phys + rsvd);
    heap_arena.size = PAGE_ALIGN(mmu_initial_mappings[0].size - rsvd);
}
#endif

static void platform_update_pagetable(void)
{
    struct map_range range;
    arch_flags_t access;
    map_addr_t pml4_table = (map_addr_t)paddr_to_kvaddr(x86_get_cr3());

    /* kernel code section mapping */
    access = ARCH_MMU_FLAG_PERM_RO;
    range.start_vaddr = (map_addr_t) & __code_start;
    range.start_paddr = (uint64_t)vaddr_to_paddr((void *) & __code_start);
    range.size =
        ((map_addr_t) & __code_end) - ((map_addr_t) & __code_start);
    x86_mmu_map_range(pml4_table, &range, access);

    /* kernel data section mapping */
    access = 0;
#if defined(ARCH_X86_64) || defined(PAE_MODE_ENABLED)
    access |= ARCH_MMU_FLAG_PERM_NO_EXECUTE;
#endif
    range.start_vaddr = (map_addr_t) & __data_start;
    range.start_paddr = (uint64_t)vaddr_to_paddr((void *) & __data_start);
    range.size =
        ((map_addr_t) & __data_end) - ((map_addr_t) & __data_start);
    x86_mmu_map_range(pml4_table, &range, access);

    /* kernel rodata section mapping */
    access = ARCH_MMU_FLAG_PERM_RO;
#if defined(ARCH_X86_64) || defined(PAE_MODE_ENABLED)
    access |= ARCH_MMU_FLAG_PERM_NO_EXECUTE;
#endif
    range.start_vaddr = (map_addr_t) & __rodata_start;
    range.start_paddr = (uint64_t)vaddr_to_paddr((void *) & __rodata_start);
    range.size =
        ((map_addr_t) & __rodata_end) - ((map_addr_t) & __rodata_start);
    x86_mmu_map_range(pml4_table, &range, access);

    /* kernel bss section and kernel heap mappings */
    access = 0;
#ifdef ARCH_X86_64
    access |= ARCH_MMU_FLAG_PERM_NO_EXECUTE;
#endif
    range.start_vaddr =  (map_addr_t) & __bss_start;
    range.start_paddr = (uint64_t)vaddr_to_paddr((void *) & __bss_start);
    range.size = ((map_addr_t) &__bss_end) - ((map_addr_t) & __bss_start);
    x86_mmu_map_range(pml4_table, &range, access);

    /* Mapping lower boundary to kernel start */
    access = ARCH_MMU_FLAG_PERM_NO_EXECUTE;
    range.start_vaddr = (map_addr_t)paddr_to_kvaddr(mmu_initial_mappings[0].phys);
    range.start_paddr = mmu_initial_mappings[0].phys;
    range.size = vaddr_to_paddr((void *)&_start) - mmu_initial_mappings[0].phys;
    x86_mmu_map_range(pml4_table, &range, access | ARCH_MMU_FLAG_NS);

    /* Mapping upper boundary to target maxium memory size */
    map_addr_t va = (map_addr_t)&_end;
    range.start_vaddr = (map_addr_t)PAGE_ALIGN(va);
    range.start_paddr = (uint64_t)vaddr_to_paddr((void *)PAGE_ALIGN(va));
    range.size = ((map_addr_t)(mmu_initial_mappings[0].phys + mmu_initial_mappings[0].size) - range.start_paddr);
    x86_mmu_map_range(pml4_table, &range, access | ARCH_MMU_FLAG_NS);
}

void platform_init_mmu_mappings(void)
{
    /* Flush TLB */
    x86_set_cr3(x86_get_cr3());
}

void clear_sensitive_data(void)
{
    if(g_sec_info->size_of_this_struct > 0) {
        memset(g_sec_info, 0, g_sec_info->size_of_this_struct);
        vmm_free_region(vmm_get_kernel_aspace(), (vaddr_t)g_sec_info);
    }
}

void smc_init(void)
{
    int vmm_id;

    vmm_id = detect_vmm();
    if (vmm_id == VMM_ID_EVMM) {
        make_smc_vmcall = make_smc_vmcall_evmm;
    } else if (vmm_id == VMM_ID_ACRN) {
        make_smc_vmcall = make_smc_vmcall_acrn;
    } else {
        dprintf(CRITICAL, "Trusty is not yet supported on Current VMM!\n");
        ASSERT(0);
    }

    dprintf(INFO, "Detected VMM: signature=%s\n", vmm_signature[vmm_id]);
}

/*
* TODO: need to enhance the panic handler
* currently, if we got panic in boot stage, the behavior
* is not expect, it will failed with SMC call since Android
* not started yet.
*/
static void platform_heap_init(void)
{
    mmu_initial_mappings[0].phys = entry_phys;
    mmu_initial_mappings[0].virt = (vaddr_t)&_start;
    mmu_initial_mappings[0].virt -= KERNEL_LOAD_OFFSET;

    mmu_initial_mappings[1].phys += entry_phys;
    mmu_initial_mappings[1].virt += entry_phys;
}

void platform_early_init(void)
{
    /* initialize the heap */
    platform_heap_init();

    /* initialize the interrupt controller */
    platform_init_interrupts();

    /* initialize the timer */
    platform_init_timer();

#ifdef WITH_KERNEL_VM
    heap_arena_init();
    pmm_add_arena(&heap_arena);
#endif

#if PRINT_USE_MMIO
    init_uart();
#endif

    local_apic_init();
}

static inline void __cpuid(uint64_t cpu_info[4], uint64_t leaf, uint64_t subleaf)
{
    __asm__ __volatile__ (
        "pushq %%rbx;" /* save the ebx */
        "cpuid;"
        "mov %%rbx, %1;" /* save what cpuid just put in ebx */
        "popq %%rbx;" /* restore the old ebx */
        : "=a" (cpu_info[0]),
          "=r" (cpu_info[1]),
          "=c" (cpu_info[2]),
          "=d" (cpu_info[3])
        : "a" (leaf), "c" (subleaf)
        : "cc"
        );
}

static inline bool is_sep_support(uint64_t val)
{
    return !!BITMAP_GET(val, SEP_BIT);
}

static inline bool is_family_6_support(uint64_t val)
{
    if ((GET_FAMILY_ID(val) == 0x6)
            && (GET_MODEL(val) < 0x3)
            && (GET_STEPPING_ID(val) < 0x3))
        return false;

    return true;
}

static bool is_sysenter_support(void)
{
    uint64_t info[4];

    /* CPUID leaf:1 subleaf:0 */
    __cpuid(info, 1, 0);

    /* CPUID: An OS that qualifies the SEP flag must also qualify the processor
     * family and model to ensure that the SYSENTER/SYSEXIT instructions are actually present
     */
    dprintf(SPEW, "SEP: 0x%x,Family_ID: 0x%llx,"
        "Model: 0x%llx,Stepping_ID: 0x%llx\n",
        is_sep_support(info[3]), GET_FAMILY_ID(info[0]),
        GET_MODEL(info[0]), GET_STEPPING_ID(info[0]));
    if (!is_sep_support(info[3]))
        return false;

    if (!is_family_6_support(info[0]))
        return false;

    return true;
}

static void prepare_secinfo_region(void)
{
    void *vaddr;
    status_t err;

    err = vmm_alloc(vmm_get_kernel_aspace(),
                    "sec",
                    sizeof(device_sec_info_t),
                    &vaddr,
                    PAGE_SIZE_SHIFT,
                    0,
                    ARCH_MMU_FLAG_PERM_NO_EXECUTE | ARCH_MMU_FLAG_UNCACHED);

    if (err) {
        panic("Failed to allocate memory for sec info, erro:%d!\n", err);
        return;
    }

    make_get_secinfo_vmcall(vaddr);
    g_sec_info = vaddr;
}

void platform_init(void)
{
    /* MMU init for x86 Archs done after the heap is setup */
   // arch_mmu_init_percpu();

    prepare_secinfo_region();

    smc_init();

#if ATTKB_HECI
    cse_init();
#endif
    if (!is_sysenter_support())
        panic("Sysenter unsupport!\n");

    platform_init_mmu_mappings();
    x86_mmu_init();

    platform_update_pagetable();
#ifdef SPI_CONTROLLER
    spi_mmu_init();
#endif
}
