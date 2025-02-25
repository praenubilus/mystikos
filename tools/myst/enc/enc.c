// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>

#include <openenclave/enclave.h>

#include <elf.h>
#include <myst/args.h>
#include <myst/buf.h>
#include <myst/eraise.h>
#include <myst/file.h>
#include <myst/kernel.h>
#include <myst/kstack.h>
#include <myst/mmanutils.h>
#include <myst/mount.h>
#include <myst/process.h>
#include <myst/ramfs.h>
#include <myst/regions.h>
#include <myst/reloc.h>
#include <myst/shm.h>
#include <myst/strings.h>
#include <myst/syscall.h>
#include <myst/tcall.h>
#include <myst/thread.h>
#include <myst/trace.h>
#include <signal.h>

#include "../config.h"
#include "../kargs.h"
#include "../shared.h"
#include "myst_t.h"

#define IRETFRAME_Rip 0
#define IRETFRAME_SegCs IRETFRAME_Rip + 8
#define IRETFRAME_EFlags IRETFRAME_SegCs + 8
#define IRETFRAME_Rsp IRETFRAME_EFlags + 8

static myst_kernel_args_t _kargs;
static mcontext_t _mcontext;
static bool _trace_syscalls = false;

extern const void* __oe_get_heap_base(void);

long myst_tcall_isatty(int fd);

long _exception_handler_syscall(long n, long params[6])
{
    return (*_kargs.myst_syscall)(n, params);
}

static void _oe_context_to_mcontext(oe_context_t* oe_context, mcontext_t* mc)
{
    memset(mc, 0, sizeof(mcontext_t));
    mc->gregs[REG_R8] = (int64_t)(oe_context->r8);
    mc->gregs[REG_R9] = (int64_t)(oe_context->r9);
    mc->gregs[REG_R10] = (int64_t)(oe_context->r10);
    mc->gregs[REG_R11] = (int64_t)(oe_context->r11);
    mc->gregs[REG_R12] = (int64_t)(oe_context->r12);
    mc->gregs[REG_R13] = (int64_t)(oe_context->r13);
    mc->gregs[REG_R14] = (int64_t)(oe_context->r14);
    mc->gregs[REG_R15] = (int64_t)(oe_context->r15);

    mc->gregs[REG_RSI] = (int64_t)(oe_context->rsi);
    mc->gregs[REG_RDI] = (int64_t)(oe_context->rdi);
    mc->gregs[REG_RBP] = (int64_t)(oe_context->rbp);
    mc->gregs[REG_RSP] = (int64_t)(oe_context->rsp);
    mc->gregs[REG_RIP] = (int64_t)(oe_context->rip);

    mc->gregs[REG_RAX] = (int64_t)(oe_context->rax);
    mc->gregs[REG_RBX] = (int64_t)(oe_context->rbx);
    mc->gregs[REG_RCX] = (int64_t)(oe_context->rcx);
    mc->gregs[REG_RDX] = (int64_t)(oe_context->rdx);

    mc->gregs[REG_EFL] = (int64_t)(oe_context->flags);
}

static void _mcontext_to_oe_context(mcontext_t* mc, oe_context_t* oe_context)
{
    oe_context->r8 = (uint64_t)(mc->gregs[REG_R8]);
    oe_context->r9 = (uint64_t)(mc->gregs[REG_R9]);
    oe_context->r10 = (uint64_t)(mc->gregs[REG_R10]);
    oe_context->r11 = (uint64_t)(mc->gregs[REG_R11]);
    oe_context->r12 = (uint64_t)(mc->gregs[REG_R12]);
    oe_context->r13 = (uint64_t)(mc->gregs[REG_R13]);
    oe_context->r14 = (uint64_t)(mc->gregs[REG_R14]);
    oe_context->r15 = (uint64_t)(mc->gregs[REG_R15]);

    oe_context->rsi = (uint64_t)(mc->gregs[REG_RSI]);
    oe_context->rdi = (uint64_t)(mc->gregs[REG_RDI]);
    oe_context->rbp = (uint64_t)(mc->gregs[REG_RBP]);
    oe_context->rsp = (uint64_t)(mc->gregs[REG_RSP]);
    oe_context->rip = (uint64_t)(mc->gregs[REG_RIP]);

    oe_context->rax = (uint64_t)(mc->gregs[REG_RAX]);
    oe_context->rbx = (uint64_t)(mc->gregs[REG_RBX]);
    oe_context->rcx = (uint64_t)(mc->gregs[REG_RCX]);
    oe_context->rdx = (uint64_t)(mc->gregs[REG_RDX]);

    oe_context->flags = (uint64_t)(mc->gregs[REG_EFL]);
}

static uint64_t _forward_exception_as_signal_to_kernel(
    oe_exception_record_t* oe_exception_record)
{
    uint32_t oe_exception_code = oe_exception_record->code;
    oe_context_t* oe_context = oe_exception_record->context;
    siginfo_t siginfo = {0};
    _oe_context_to_mcontext(oe_context, &_mcontext);

    // Kernel should be the ultimate handler of #PF, #GP, #MF, and #UD.
    // If we are still alive after kernel handling, it means kernel
    // wanted the execution to continue.
    if (oe_exception_code == OE_EXCEPTION_ILLEGAL_INSTRUCTION)
    {
        siginfo.si_code = SI_KERNEL;
        siginfo.si_signo = SIGILL;
        (*_kargs.myst_handle_host_signal)(&siginfo, &_mcontext);
        _mcontext_to_oe_context(&_mcontext, oe_context);
        return OE_EXCEPTION_CONTINUE_EXECUTION;
    }
    if (oe_exception_code == OE_EXCEPTION_PAGE_FAULT)
    {
        // ATTN: Use the following rule to determine the si_code, which
        // may be different from the behavior of the Linux kernel.
        if (oe_exception_record->error_code & OE_SGX_PAGE_FAULT_PK_FLAG)
            siginfo.si_code = SEGV_PKUERR;
        else if (oe_exception_record->error_code & OE_SGX_PAGE_FAULT_P_FLAG)
            siginfo.si_code = SEGV_ACCERR;
        else
            siginfo.si_code = SEGV_MAPERR;
        siginfo.si_signo = SIGSEGV;
        // The faulting address is only avaiable on icelake. The
        // mystikos-specific OE runtime also simulates the #PF on
        // coffeelake when the enclave is in debug mode.
        // Note that the si_addr in the simulated #PF always has the
        // lower 12 bits cleared.
        siginfo.si_addr = (void*)oe_exception_record->faulting_address;
        (*_kargs.myst_handle_host_signal)(&siginfo, &_mcontext);
        _mcontext_to_oe_context(&_mcontext, oe_context);
        return OE_EXCEPTION_CONTINUE_EXECUTION;
    }
    if (oe_exception_code == OE_EXCEPTION_ACCESS_VIOLATION)
    {
        // #GP can only be delivered on icelake.
        siginfo.si_code = SEGV_ACCERR;
        siginfo.si_signo = SIGSEGV;
        // `si_addr` is always null for #GP.
        siginfo.si_addr = NULL;
        (*_kargs.myst_handle_host_signal)(&siginfo, &_mcontext);
        _mcontext_to_oe_context(&_mcontext, oe_context);
        return OE_EXCEPTION_CONTINUE_EXECUTION;
    }
    if (oe_exception_code == OE_EXCEPTION_X87_FLOAT_POINT)
    {
        // ATTN: Consider implementing accurate si-code for
        // OE_EXCEPTION_X87_FLOAT_POINT
        siginfo.si_code = FPE_FLTINV;
        siginfo.si_signo = SIGFPE;
        (*_kargs.myst_handle_host_signal)(&siginfo, &_mcontext);
        _mcontext_to_oe_context(&_mcontext, oe_context);
        return OE_EXCEPTION_CONTINUE_EXECUTION;
    }
    if (oe_exception_code == OE_EXCEPTION_DIVIDE_BY_ZERO)
    {
        siginfo.si_code = FPE_INTDIV;
        siginfo.si_signo = SIGFPE;
        (*_kargs.myst_handle_host_signal)(&siginfo, &_mcontext);
        _mcontext_to_oe_context(&_mcontext, oe_context);
        return OE_EXCEPTION_CONTINUE_EXECUTION;
    }

    // ATTN: Consider forwarding OE_EXCEPTION_BOUND_OUT_OF_RANGE,
    // OE_EXCEPTION_ACCESS_VIOLATION, OE_EXCEPTION_MISALIGNMENT,
    // OE_EXCEPTION_SIMD_FLOAT_POINT as signal.
    // Delegate unhandled hardware exceptions to other vector handlers.
    return OE_EXCEPTION_CONTINUE_SEARCH;
}

extern volatile const oe_sgx_enclave_properties_t oe_enclave_properties_sgx;

static size_t _get_num_tcs(void)
{
    return oe_enclave_properties_sgx.header.size_settings.num_tcs;
}

int myst_setup_clock(struct clock_ctrl*);

static void _sanitize_xsave_area_fields(uint64_t* rbx, uint64_t* rcx)
{
    assert(rbx && rcx);
    /* replace XSAVE/XRSTOR save area size with fixed large value of 4096,
    to protect against spoofing attacks from untrusted host.
    If host returns smaller xsave area than required, this can cause a buffer
    overflow at context switch time.
    We believe value of 4096 should be sufficient for forseeable future. */
    if (*rbx < 4096)
        *rbx = 4096;
    if (*rcx < 4096)
        *rcx = 4096;
}

#define COLOR_GREEN "\e[32m"
#define COLOR_RESET "\e[0m"

#define RDTSC_OPCODE 0x310F
#define CPUID_OPCODE 0xA20F
#define IRETQ_OPCODE 0xCF48
#define SYSCALL_OPCODE 0x050F

typedef struct _opcode_pair
{
    long opcode;
    const char* str;
} opcode_pair_t;

static opcode_pair_t _opcode_pairs[] = {
    {RDTSC_OPCODE, "rdtsc"},
    {CPUID_OPCODE, "cpuid"},
    {IRETQ_OPCODE, "iretq"},
    {SYSCALL_OPCODE, "syscall"},
};

static size_t _n_pairs = sizeof(_opcode_pairs) / sizeof(_opcode_pairs[0]);
const char* opcode_str(long n)
{
    for (size_t i = 0; i < _n_pairs; i++)
    {
        if (n == _opcode_pairs[i].opcode)
            return _opcode_pairs[i].str;
    }

    return "unknown";
}

__attribute__((format(printf, 2, 3))) static void _exception_handler_strace(
    long n,
    const char* fmt,
    ...)
{
    if (_trace_syscalls)
    {
        char null_char = '\0';
        char* buf = &null_char;
        const bool isatty = myst_tcall_isatty(STDERR_FILENO) == 1l;
        const char* green = isatty ? COLOR_GREEN : "";
        const char* reset = isatty ? COLOR_RESET : "";

        if (fmt)
        {
            const size_t buf_size = 1024;

            if (!(buf = malloc(buf_size)))
            {
                fprintf(stderr, "out of memory\n");
                assert(0);
            }
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(buf, buf_size, fmt, ap);
            va_end(ap);
        }

        fprintf(
            stderr,
            "[exception handler] %s%s%s(%s)\n",
            green,
            opcode_str(n),
            reset,
            buf);

        if (buf != &null_char)
            free(buf);
    }
}

/* Handle illegal SGX instructions */
static uint64_t _vectored_handler(oe_exception_record_t* er)
{
    const uint16_t opcode = *((uint16_t*)er->context->rip);

    if (er->code == OE_EXCEPTION_ILLEGAL_INSTRUCTION)
    {
        _exception_handler_strace(opcode, NULL);
        switch (opcode)
        {
            case RDTSC_OPCODE:
            {
                uint32_t rax = 0;
                uint32_t rdx = 0;

                /* Ask host to execute RDTSC instruction */
                if (myst_rdtsc_ocall(&rax, &rdx) != OE_OK)
                {
                    fprintf(stderr, "myst_rdtsc_ocall() failed\n");
                    assert(false);
                    return OE_EXCEPTION_CONTINUE_SEARCH;
                }

                er->context->rax = rax;
                er->context->rdx = rdx;

                /* Skip over the illegal instruction. */
                er->context->rip += 2;

                return OE_EXCEPTION_CONTINUE_EXECUTION;
                break;
            }
            case CPUID_OPCODE:
            {
                uint32_t rax = 0xaa;
                uint32_t rbx = 0xbb;
                uint32_t rcx = 0xcc;
                uint32_t rdx = 0xdd;
                bool is_xsave_subleaf_zero =
                    (er->context->rax == 0xd && er->context->rcx == 0);

                if (er->context->rax != 0xff)
                {
                    _exception_handler_strace(
                        opcode,
                        "rax= 0x%lx rcx= 0x%lx",
                        er->context->rax,
                        er->context->rcx);
                    myst_cpuid_ocall(
                        (uint32_t)er->context->rax, /* leaf */
                        (uint32_t)er->context->rcx, /* subleaf */
                        &rax,
                        &rbx,
                        &rcx,
                        &rdx);
                }

                er->context->rax = rax;
                er->context->rbx = rbx;
                er->context->rcx = rcx;
                er->context->rdx = rdx;

                if (is_xsave_subleaf_zero)
                    _sanitize_xsave_area_fields(
                        &er->context->rbx, &er->context->rcx);

                /* Skip over the illegal instruction. */
                er->context->rip += 2;

                return OE_EXCEPTION_CONTINUE_EXECUTION;
                break;
            }
            case IRETQ_OPCODE:
            {
                // Restore RSP, RIP, EFLAGS from the stack. CS and SS are not
                // applicable for sgx applications, and restoring them triggers
                // #UD.

                er->context->flags =
                    *(uint64_t*)(er->context->rsp + IRETFRAME_EFlags);
                er->context->rip =
                    *(uint64_t*)(er->context->rsp + IRETFRAME_Rip);
                er->context->rsp =
                    *(uint64_t*)(er->context->rsp + IRETFRAME_Rsp);

                return OE_EXCEPTION_CONTINUE_EXECUTION;
                break;
            }
            case SYSCALL_OPCODE:
            {
                long params[6] = {0};

                // SYSCALL saves RIP (next instruction after SYSCALL) to RCX and
                // SYSRET restors the RIP from RCX
                er->context->rcx = er->context->rip + 2;
                er->context->rip = er->context->rcx;
                // SYSCALL saves RFLAGS into R11 and clears in RFLAGS every bit
                // corresponding to a bit that is set in the IA32_FMASK MSR, for
                // CPU operations. No need to emulate RFLAGS value here.
                // SYSRET loads (r11 & 0x3C7FD7) | 2 to RFLAG
                er->context->r11 = er->context->flags;
                er->context->flags = (er->context->r11 & 0x3C7FD7) | 2;

                params[0] = (long)er->context->rdi;
                params[1] = (long)er->context->rsi;
                params[2] = (long)er->context->rdx;
                params[3] = (long)er->context->r10;
                params[4] = (long)er->context->r8;
                params[5] = (long)er->context->r9;

                // syscall number is in RAX. SYSRET sets RAX.
                er->context->rax = (uint64_t)_exception_handler_syscall(
                    (long)er->context->rax, params);

                // If the specific syscall is not supported in Mystikos, the
                // exception handler will cause abort.
                return OE_EXCEPTION_CONTINUE_EXECUTION;
                break;
            }
            default:
                break;
        }
    }

    return _forward_exception_as_signal_to_kernel(er);
}

static bool _is_allowed_env_variable(
    const config_parsed_data_t* config,
    const char* env)
{
    for (size_t i = 0; i < config->host_environment_variables_count; i++)
    {
        const char* allowed = config->host_environment_variables[i];
        size_t len = strlen(allowed);

        if (strncmp(env, allowed, len) == 0 && env[len] == '=')
            return true;
    }

    return false;
}

const void* __oe_get_enclave_start_address(void);
size_t __oe_get_enclave_size(void);

volatile int myst_enter_ecall_lock = 0;

/* return 0 if OE is in SGX debug mode (else return -1) */
static int _test_oe_debug_mode(void)
{
    int ret = -1;
    uint8_t* buf = NULL;
    size_t buf_size;
    oe_report_t report;

    if (oe_get_report_v2(0, NULL, 0, NULL, 0, &buf, &buf_size) != OE_OK)
        goto done;

    if (oe_parse_report(buf, buf_size, &report) != OE_OK)
        goto done;

    if (!(report.identity.attributes & OE_REPORT_ATTRIBUTES_DEBUG))
        goto done;

    ret = 0;

done:

    if (buf)
        oe_free_report(buf);

    return ret;
}

struct enter_arg
{
    struct myst_options* options;
    struct myst_shm* shared_memory;
    const void* argv_data;
    size_t argv_size;
    const void* envp_data;
    size_t envp_size;
    const void* mount_mappings_data;
    size_t mount_mappings_size;
    uint64_t event;
    pid_t target_tid;
    uint64_t start_time_sec;
    uint64_t start_time_nsec;
    const void* enter_stack;
    size_t enter_stack_size;
};

static long _enter(void* arg_)
{
    long ret = -1;
    struct enter_arg* arg = (struct enter_arg*)arg_;
    struct myst_options* options = arg->options;
    struct myst_shm* shared_memory = arg->shared_memory;
    const void* argv_data = arg->argv_data;
    size_t argv_size = arg->argv_size;
    const void* envp_data = arg->envp_data;
    size_t envp_size = arg->envp_size;
    uint64_t event = arg->event;
    pid_t target_tid = arg->target_tid;
    bool trace_errors = false;
    bool shell_mode = false;
    bool debug_symbols = false;
    bool memcheck = false;
    bool nobrk = false;
    bool perf = false;
    bool report_native_tids = false;
    size_t max_affinity_cpus = options ? options->max_affinity_cpus : 0;
    size_t main_stack_size = options ? options->main_stack_size : 0;
    const char* rootfs = NULL;
    config_parsed_data_t parsed_config;
    bool have_config = false;
    myst_args_t args;
    myst_args_t env;
    const char* cwd = "/";       // default to root dir
    const char* hostname = NULL; // kernel has a default
    const uint8_t* enclave_image_base;
    size_t enclave_image_size;
    const Elf64_Ehdr* ehdr;
    const char target[] = "MYST_TARGET=sgx";
    const bool tee_debug_mode = (_test_oe_debug_mode() == 0);
    myst_fork_mode_t fork_mode = options ? options->fork_mode : myst_fork_none;
    bool unhandled_syscall_enosys =
        options ? options->unhandled_syscall_enosys
                : false; // default terminate with myst_panic

    memset(&parsed_config, 0, sizeof(parsed_config));

    if (!argv_data || !argv_size || !envp_data || !envp_size)
        goto done;

    memset(&args, 0, sizeof(args));
    memset(&env, 0, sizeof(env));

    /* Get the enclave base address */
    if (!(enclave_image_base = __oe_get_enclave_start_address()))
    {
        fprintf(stderr, "__oe_get_enclave_start_address() failed\n");
        assert(0);
    }

    /* Get the enclave size */
    enclave_image_size = __oe_get_enclave_size();

    /* Get the config region */
    {
        myst_region_t r;
        const void* regions = __oe_get_heap_base();

        if (myst_region_find(regions, MYST_REGION_CONFIG, &r) == 0)
        {
            if (parse_config_from_buffer(r.data, r.size, &parsed_config) != 0)
            {
                fprintf(stderr, "failed to parse configuration\n");
                assert(0);
            }
            have_config = true;
        }
    }

    if (have_config && !parsed_config.allow_host_parameters)
    {
        if (myst_args_init(&args) != 0)
            goto done;

        if (myst_args_append1(&args, parsed_config.application_path) != 0)
            goto done;

        if (myst_args_append(
                &args,
                (const char**)parsed_config.application_parameters,
                parsed_config.application_parameters_count) != 0)
        {
            goto done;
        }
    }
    else
    {
        if (myst_args_unpack(&args, argv_data, argv_size) != 0)
            goto done;
    }

    // Need to handle config to environment
    // in the mean time we will just pull from the host
    if (have_config)
    {
        myst_args_init(&env);

        // append all enclave-side environment variables first
        if (myst_args_append(
                &env,
                (const char**)parsed_config.enclave_environment_variables,
                parsed_config.enclave_environment_variables_count) != 0)
        {
            goto done;
        }

        // now include host-side environment variables that are allowed
        if (parsed_config.host_environment_variables &&
            parsed_config.host_environment_variables_count)
        {
            myst_args_t tmp;

            if (myst_args_unpack(&tmp, envp_data, envp_size) != 0)
                goto done;

            for (size_t i = 0; i < tmp.size; i++)
            {
                if (_is_allowed_env_variable(&parsed_config, tmp.data[i]))
                {
                    if (myst_args_append1(&env, tmp.data[i]) != 0)
                    {
                        free(tmp.data);
                        goto done;
                    }
                }
            }

            free(tmp.data);
        }
    }
    else
    {
        if (myst_args_unpack(&env, envp_data, envp_size) != 0)
            goto done;
    }

    // Override current working directory if present in config
    if (have_config && parsed_config.cwd)
    {
        cwd = parsed_config.cwd;
    }

    // Override current working directory if present in config
    if (have_config && parsed_config.hostname)
    {
        hostname = parsed_config.hostname;
    }

    // Override max affinity if present in config
    if (have_config && parsed_config.max_affinity_cpus)
    {
        max_affinity_cpus = parsed_config.max_affinity_cpus;
    }

    // Override main stack size if present in config
    if (have_config && parsed_config.main_stack_size)
    {
        main_stack_size = parsed_config.main_stack_size;
    }

    // record the configuration for which fork mode
    if (have_config && parsed_config.fork_mode)
    {
        fork_mode = parsed_config.fork_mode;
    }

    if (have_config && parsed_config.no_brk)
    {
        nobrk = options->nobrk = true;
    }

    // record the configuration for which termination mode
    if (have_config)
    {
        unhandled_syscall_enosys = parsed_config.unhandled_syscall_enosys;
    }

    /* Inject the MYST_TARGET environment variable */
    {
        const char val[] = "MYST_TARGET=";

        for (size_t i = 0; i < env.size; i++)
        {
            if (strncmp(env.data[i], val, sizeof(val) - 1) == 0)
            {
                fprintf(stderr, "environment already contains %s", val);
                goto done;
            }
        }

        myst_args_append1(&env, "MYST_TARGET=sgx");
    }

    /* Add mount source paths to config read mount points */
    {
        myst_args_t tmp = {0};

        if (myst_args_unpack(
                &tmp, arg->mount_mappings_data, arg->mount_mappings_size) != 0)
        {
            fprintf(stderr, "Failed to unpack mapping parameters\n");
            goto done;
        }

        if (have_config && (!myst_merge_mount_mapping_and_config(
                                &parsed_config.mounts, &tmp) ||
                            !myst_validate_mount_config(&parsed_config.mounts)))
        {
            fprintf(
                stderr,
                "Failed to merge mounting configuration with command line "
                "mount parameters\n");
            goto done;
        }
    }

    if (options)
    {
        // _trace_syscalls is used by vectored exception handler tracing,
        // disable it if tee_debug_mode is false
        _trace_syscalls = tee_debug_mode ? options->trace_syscalls : false;
        // if tee_debug_mode is false, these options are disabled by the
        // kernel upon entry.
        trace_errors = tee_debug_mode ? options->trace_errors : false;
        shell_mode = tee_debug_mode ? options->shell_mode : false;
        debug_symbols = tee_debug_mode ? options->debug_symbols : false;
        memcheck = tee_debug_mode ? options->memcheck : false;
        nobrk = options->nobrk;
        perf = tee_debug_mode ? options->perf : false;

        report_native_tids =
            tee_debug_mode ? options->report_native_tids : false;

        /* rootfs buffer content set by the host side. Max length of the string
         * is PATH_MAX-1. Enforce NULL terminator at the end of the buffer.
         */
        if (strnlen(options->rootfs, PATH_MAX) == PATH_MAX)
        {
            fprintf(stderr, "rootfs path too long (> %u)\n", PATH_MAX);
            goto done;
        }

        rootfs = options->rootfs;
    }

    /* Setup the vectored exception handler */
    if (oe_add_vectored_exception_handler(true, _vectored_handler) != OE_OK)
    {
        fprintf(stderr, "oe_add_vectored_exception_handler() failed\n");
        assert(0);
    }

    if (myst_setup_clock(shared_memory->clock))
    {
        fprintf(stderr, "myst_setup_clock() failed\n");
        assert(0);
    }

    /* Enter the kernel image */
    {
        myst_kernel_entry_t entry;
        const void* regions_end = __oe_get_heap_base();
        char err[256];

        init_kernel_args(
            &_kargs,
            target,
            (int)args.size,
            args.data,
            (int)env.size,
            env.data,
            cwd,
            &options->host_enc_uid_gid_mappings,
            &parsed_config.mounts,
            hostname,
            regions_end,
            enclave_image_base, /* image_data */
            enclave_image_size, /* image_size */
            _get_num_tcs(),     /* max threads */
            trace_errors,
            _trace_syscalls,
            false, /* have_syscall_instruction */
            tee_debug_mode,
            event, /* thread_event */
            target_tid,
            max_affinity_cpus,
            fork_mode,
            myst_tcall,
            rootfs,
            err,
            unhandled_syscall_enosys,
            sizeof(err));

        _kargs.shell_mode = shell_mode;
        _kargs.debug_symbols = debug_symbols;
        _kargs.memcheck = memcheck;
        _kargs.nobrk = nobrk;
        _kargs.perf = perf;
        _kargs.start_time_sec = arg->start_time_sec;
        _kargs.start_time_nsec = arg->start_time_nsec;
        _kargs.report_native_tids = report_native_tids;
        _kargs.enter_stack = arg->enter_stack;
        _kargs.enter_stack_size = arg->enter_stack_size;
        _kargs.main_stack_size =
            main_stack_size ? main_stack_size : MYST_PROCESS_INIT_STACK_SIZE;

        /* whether user-space FSGSBASE instructions are supported */
        _kargs.have_fsgsbase_instructions = options->have_fsgsbase_instructions;

        /* set ehdr and verify that the kernel is an ELF image */
        {
            ehdr = (const Elf64_Ehdr*)_kargs.kernel_data;
            const uint8_t ident[] = {0x7f, 'E', 'L', 'F'};

            if (memcmp(ehdr->e_ident, ident, sizeof(ident)) != 0)
            {
                fprintf(stderr, "bad kernel image\n");
                assert(0);
            }
        }

        /* Resolve the the kernel entry point */
        entry =
            (myst_kernel_entry_t)((uint8_t*)_kargs.kernel_data + ehdr->e_entry);

        if ((uint8_t*)entry < (uint8_t*)_kargs.kernel_data ||
            (uint8_t*)entry >=
                (uint8_t*)_kargs.kernel_data + _kargs.kernel_size)
        {
            fprintf(stderr, "kernel entry point is out of bounds\n");
            assert(0);
        }

        ret = (*entry)(&_kargs);
    }

done:

    if (args.data)
        free(args.data);

    if (env.data)
        free(env.data);

    free_config(&parsed_config);
    return ret;
}

int myst_enter_ecall(
    struct myst_options* options,
    struct myst_shm* shared_memory,
    const void* argv_data,
    size_t argv_size,
    const void* envp_data,
    size_t envp_size,
    const void* mount_mappings,
    size_t mount_mappings_size,
    uint64_t event,
    pid_t target_tid,
    uint64_t start_time_sec,
    uint64_t start_time_nsec)
{
    struct enter_arg arg = {
        .options = options,
        .shared_memory = shared_memory,
        .argv_data = argv_data,
        .argv_size = argv_size,
        .envp_data = envp_data,
        .envp_size = envp_size,
        .mount_mappings_data = mount_mappings,
        .mount_mappings_size = mount_mappings_size,
        .event = event,
        .target_tid = target_tid,
        .start_time_sec = start_time_sec,
        .start_time_nsec = start_time_nsec,
    };

    /* prevent this function from being called more than once */
    if (__sync_fetch_and_add(&myst_enter_ecall_lock, 1) != 0)
    {
        myst_enter_ecall_lock = 1; // stop this from wrapping
        return -1;
    }

    const void* regions = __oe_get_heap_base();
    myst_region_t reg;

    /* find the stack for entering the kernel */
    if (myst_region_find(regions, MYST_REGION_KERNEL_ENTER_STACK, &reg) != 0)
        return -1;

    uint8_t* stack = (uint8_t*)reg.data + reg.size;

    arg.enter_stack = reg.data;
    arg.enter_stack_size = reg.size;

    /* avoid using the tiny TCS stack */
    return (int)myst_call_on_stack(stack, _enter, &arg);
}

long myst_run_thread_ecall(uint64_t cookie, uint64_t event, pid_t target_tid)
{
    return myst_run_thread(cookie, event, target_tid);
}

/* This overrides the weak version in libmystkernel.a */
long myst_tcall_add_symbol_file(
    const void* file_data,
    size_t file_size,
    const void* text,
    size_t text_size,
    const char* enclave_rootfs_path)
{
    long ret = 0;
    int retval;

    if (!text || !text_size)
        ERAISE(-EINVAL);

    if (myst_add_symbol_file_ocall(
            &retval,
            file_data,
            file_size,
            text,
            text_size,
            enclave_rootfs_path) != OE_OK)
    {
        ERAISE(-EINVAL);
    }

done:

    return ret;
}

/* This overrides the weak version in libmystkernel.a */
long myst_tcall_load_symbols(void)
{
    long ret = 0;
    int retval;

    if (myst_load_symbols_ocall(&retval) != OE_OK || retval != 0)
        ERAISE(-EINVAL);

done:
    return ret;
}

/* This overrides the weak version in libmystkernel.a */
long myst_tcall_unload_symbols(void)
{
    long ret = 0;
    int retval;

    if (myst_unload_symbols_ocall(&retval) != OE_OK || retval != 0)
        ERAISE(-EINVAL);

done:
    return ret;
}

/* This overrides the weak version in libmystkernel.a */
long myst_tcall_isatty(int fd)
{
    long ret;

    if (myst_syscall_isatty_ocall(&ret, fd) != OE_OK)
        return -EINVAL;

    return (long)ret;
}

long myst_tcall_create_thread(uint64_t cookie)
{
    long ret;

    if (myst_create_thread_ocall(&ret, cookie) != OE_OK)
        return -EINVAL;

    return (long)ret;
}

long myst_tcall_wait(uint64_t event, const struct timespec* timeout)
{
    long retval = -EINVAL;
    const struct myst_timespec* to = (const struct myst_timespec*)timeout;

    if (myst_wait_ocall(&retval, event, to) != OE_OK)
        return -EINVAL;

    return retval;
}

long myst_tcall_wake(uint64_t event)
{
    long retval = -EINVAL;

    if (myst_wake_ocall(&retval, event) != OE_OK)
        return -EINVAL;

    return retval;
}

long myst_tcall_wake_wait(
    uint64_t waiter_event,
    uint64_t self_event,
    const struct timespec* timeout)
{
    long retval = -EINVAL;
    const struct myst_timespec* to = (const struct myst_timespec*)timeout;

    if (myst_wake_wait_ocall(&retval, waiter_event, self_event, to) != OE_OK)
        return -EINVAL;

    return retval;
}

long myst_tcall_poll_wake(void)
{
    long r;

    if (myst_poll_wake_ocall(&r) != OE_OK)
        return -EINVAL;

    return r;
}

int myst_tcall_open_block_device(const char* path, bool read_only)
{
    int retval;

    if (myst_open_block_device_ocall(&retval, path, read_only) != OE_OK)
        return -EINVAL;

    return retval;
}

int myst_tcall_close_block_device(int blkdev)
{
    int retval;

    if (myst_close_block_device_ocall(&retval, blkdev) != OE_OK)
        return -EINVAL;

    return retval;
}

ssize_t myst_tcall_read_block_device(
    int blkdev,
    uint64_t blkno,
    struct myst_block* blocks,
    size_t num_blocks)
{
    ssize_t retval;

    if (myst_read_block_device_ocall(
            &retval, blkdev, blkno, blocks, num_blocks) != OE_OK)
    {
        return -EINVAL;
    }
    /* guard against host setting the return value greater than num_blocks */
    if (retval > (ssize_t)num_blocks)
    {
        retval = -EINVAL;
    }
    return retval;
}

int myst_tcall_write_block_device(
    int blkdev,
    uint64_t blkno,
    const struct myst_block* blocks,
    size_t num_blocks)
{
    int retval;

    if (myst_write_block_device_ocall(
            &retval, blkdev, blkno, blocks, num_blocks) != OE_OK)
    {
        return -EINVAL;
    }
    /* guard against host setting the return value greater than num_blocks */
    if (retval > (ssize_t)num_blocks)
    {
        retval = -EINVAL;
    }
    return retval;
}

int myst_load_fssig(const char* path, myst_fssig_t* fssig)
{
    int retval;

    if (!path || !fssig)
        return -EINVAL;

    if (myst_load_fssig_ocall(&retval, path, fssig) != OE_OK)
        return -EINVAL;

    if (fssig->signature_size > sizeof(fssig->signature))
    {
        memset(fssig, 0, sizeof(myst_fssig_t));
        return -EPERM;
    }

    return retval;
}

OE_SET_ENCLAVE_SGX2(
    ENCLAVE_PRODUCT_ID,
    ENCLAVE_SECURITY_VERSION,
    ENCLAVE_EXTENDED_PRODUCT_ID,
    ENCLAVE_FAMILY_ID,
    ENCLAVE_DEBUG,
    ENCLAVE_CAPTURE_PF_GP_EXCEPTIONS,
    ENCLAVE_REQUIRE_KSS,
    ENCLAVE_CREATE_ZERO_BASE_ENCLAVE,
    ENCLAVE_START_ADDRESS,
    ENCLAVE_HEAP_SIZE / OE_PAGE_SIZE,
    ENCLAVE_STACK_SIZE / OE_PAGE_SIZE,
    ENCLAVE_MAX_THREADS);
