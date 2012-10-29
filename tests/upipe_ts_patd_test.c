/*
 * Copyright (C) 2012 OpenHeadend S.A.R.L.
 *
 * Authors: Christophe Massiot
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/** @file
 * @short unit tests for TS patd module
 */

#undef NDEBUG

#include <upipe/ulog.h>
#include <upipe/ulog_std.h>
#include <upipe/uprobe.h>
#include <upipe/uprobe_print.h>
#include <upipe/umem.h>
#include <upipe/umem_alloc.h>
#include <upipe/udict.h>
#include <upipe/udict_inline.h>
#include <upipe/ubuf.h>
#include <upipe/ubuf_block.h>
#include <upipe/ubuf_block_mem.h>
#include <upipe/uref.h>
#include <upipe/uref_flow.h>
#include <upipe/uref_block_flow.h>
#include <upipe/uref_block.h>
#include <upipe/uref_clock.h>
#include <upipe/uref_std.h>
#include <upipe/upipe.h>
#include <upipe-ts/upipe_ts_patd.h>
#include <upipe-ts/uref_ts_flow.h>

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <inttypes.h>
#include <assert.h>

#include <bitstream/mpeg/psi.h>

#define UDICT_POOL_DEPTH 10
#define UREF_POOL_DEPTH 10
#define UBUF_POOL_DEPTH 10
#define ULOG_LEVEL ULOG_DEBUG

static uint8_t tsid = 42;
static unsigned int program_sum;
static unsigned int pid_sum;
static unsigned int del_program_sum;

/** definition of our uprobe */
static bool catch(struct uprobe *uprobe, struct upipe *upipe,
                  enum uprobe_event event, va_list args)
{
    switch (event) {
        case UPROBE_AERROR:
        case UPROBE_UPUMP_ERROR:
        case UPROBE_READ_END:
        case UPROBE_WRITE_END:
        case UPROBE_NEW_FLOW:
        case UPROBE_NEED_UREF_MGR:
        case UPROBE_NEED_UPUMP_MGR:
        case UPROBE_LINEAR_NEED_UBUF_MGR:
        case UPROBE_SOURCE_NEED_FLOW_NAME:
        default:
            assert(0);
            break;
        case UPROBE_READY:
            break;
        case UPROBE_TS_PATD_TSID: {
            unsigned int signature = va_arg(args, unsigned int);
            struct uref *uref = va_arg(args, struct uref *);
            unsigned int patd_tsid = va_arg(args, unsigned int);
            assert(signature == UPIPE_TS_PATD_SIGNATURE);
            assert(uref != NULL);
            fprintf(stdout, "ts probe: pipe %p detected TS ID %u\n", upipe,
                    patd_tsid);
            assert(patd_tsid == tsid);
            break;
        }
        case UPROBE_TS_PATD_NEW_PROGRAM: {
            unsigned int signature = va_arg(args, unsigned int);
            struct uref *uref = va_arg(args, struct uref *);
            unsigned int program = va_arg(args, unsigned int);
            struct uref *control = va_arg(args, struct uref *);
            assert(signature == UPIPE_TS_PATD_SIGNATURE);
            assert(uref != NULL);
            assert(control != NULL);
            uint64_t pid;
            assert(uref_ts_flow_get_pid(control, &pid));
            uref_free(control);
            program_sum -= program;
            pid_sum -= pid;
            fprintf(stdout,
                    "ts probe: pipe %p added program %u (PID %"PRIu64")\n",
                    upipe, program, pid);
            break;
        }
        case UPROBE_TS_PATD_DEL_PROGRAM: {
            unsigned int signature = va_arg(args, unsigned int);
            struct uref *uref = va_arg(args, struct uref *);
            unsigned int program = va_arg(args, unsigned int);
            assert(signature == UPIPE_TS_PATD_SIGNATURE);
            assert(uref != NULL);
            del_program_sum -= program;
            fprintf(stdout,
                    "ts probe: pipe %p deleted program %u\n", upipe, program);
            break;
        }
    }
    return true;
}

int main(int argc, char *argv[])
{
    struct umem_mgr *umem_mgr = umem_alloc_mgr_alloc();
    assert(umem_mgr != NULL);
    struct udict_mgr *udict_mgr = udict_inline_mgr_alloc(UDICT_POOL_DEPTH,
                                                         umem_mgr, -1, -1);
    assert(udict_mgr != NULL);
    struct uref_mgr *uref_mgr = uref_std_mgr_alloc(UREF_POOL_DEPTH, udict_mgr,
                                                   0);
    assert(uref_mgr != NULL);
    struct ubuf_mgr *ubuf_mgr = ubuf_block_mem_mgr_alloc(UBUF_POOL_DEPTH,
                                                         UBUF_POOL_DEPTH,
                                                         umem_mgr, -1, -1,
                                                         -1, 0);
    assert(ubuf_mgr != NULL);
    struct uprobe uprobe;
    uprobe_init(&uprobe, catch, NULL);
    struct uprobe *uprobe_print = uprobe_print_alloc(&uprobe, stdout, "test");
    assert(uprobe_print != NULL);

    struct upipe_mgr *upipe_ts_patd_mgr = upipe_ts_patd_mgr_alloc();
    assert(upipe_ts_patd_mgr != NULL);
    struct upipe *upipe_ts_patd = upipe_alloc(upipe_ts_patd_mgr, uprobe_print,
            ulog_std_alloc(stdout, ULOG_LEVEL, "ts patd"));
    assert(upipe_ts_patd != NULL);

    struct uref *uref;
    uint8_t *buffer, *pat_program;
    int size;
    uref = uref_block_flow_alloc_def(uref_mgr, "mpegtspat.");
    assert(uref != NULL);
    assert(uref_flow_set_name(uref, "source"));
    assert(upipe_input(upipe_ts_patd, uref));

    uref = uref_block_alloc(uref_mgr, ubuf_mgr,
                            PAT_HEADER_SIZE + PAT_PROGRAM_SIZE + PSI_CRC_SIZE);
    assert(uref != NULL);
    size = -1;
    assert(uref_block_write(uref, 0, &size, &buffer));
    assert(size == PAT_HEADER_SIZE + PAT_PROGRAM_SIZE + PSI_CRC_SIZE);
    pat_init(buffer);
    pat_set_length(buffer, PAT_PROGRAM_SIZE);
    pat_set_tsid(buffer, tsid);
    psi_set_version(buffer, 0);
    psi_set_current(buffer);
    psi_set_section(buffer, 0);
    psi_set_lastsection(buffer, 0);
    pat_program = pat_get_program(buffer, 0);
    patn_init(pat_program);
    patn_set_program(pat_program, 12);
    patn_set_pid(pat_program, 42);
    psi_set_crc(buffer);
    uref_block_unmap(uref, 0, size);
    assert(uref_flow_set_name(uref, "source"));
    program_sum = 12;
    pid_sum = 42;
    assert(upipe_input(upipe_ts_patd, uref));
    assert(!program_sum);
    assert(!pid_sum);

    uref = uref_block_alloc(uref_mgr, ubuf_mgr,
                            PAT_HEADER_SIZE + PAT_PROGRAM_SIZE + PSI_CRC_SIZE);
    assert(uref != NULL);
    size = -1;
    assert(uref_block_write(uref, 0, &size, &buffer));
    assert(size == PAT_HEADER_SIZE + PAT_PROGRAM_SIZE + PSI_CRC_SIZE);
    pat_init(buffer);
    pat_set_length(buffer, PAT_PROGRAM_SIZE);
    pat_set_tsid(buffer, tsid);
    psi_set_version(buffer, 1);
    psi_set_current(buffer);
    psi_set_section(buffer, 0);
    psi_set_lastsection(buffer, 0);
    pat_program = pat_get_program(buffer, 0);
    patn_init(pat_program);
    patn_set_program(pat_program, 12);
    patn_set_pid(pat_program, 12);
    psi_set_crc(buffer); /* set invalid CRC */
    patn_set_pid(pat_program, 42);
    uref_block_unmap(uref, 0, size);
    assert(uref_flow_set_name(uref, "source"));
    assert(upipe_input(upipe_ts_patd, uref));
    assert(!program_sum);
    assert(!pid_sum);

    uref = uref_block_alloc(uref_mgr, ubuf_mgr,
                            PAT_HEADER_SIZE + PAT_PROGRAM_SIZE + PSI_CRC_SIZE);
    assert(uref != NULL);
    size = -1;
    assert(uref_block_write(uref, 0, &size, &buffer));
    assert(size == PAT_HEADER_SIZE + PAT_PROGRAM_SIZE + PSI_CRC_SIZE);
    pat_init(buffer);
    pat_set_length(buffer, PAT_PROGRAM_SIZE);
    pat_set_tsid(buffer, tsid);
    psi_set_version(buffer, 2);
    // don't set current
    psi_set_section(buffer, 0);
    psi_set_lastsection(buffer, 0);
    pat_program = pat_get_program(buffer, 0);
    patn_init(pat_program);
    patn_set_program(pat_program, 12);
    patn_set_pid(pat_program, 42);
    psi_set_crc(buffer);
    uref_block_unmap(uref, 0, size);
    assert(uref_flow_set_name(uref, "source"));
    assert(upipe_input(upipe_ts_patd, uref));
    assert(!program_sum);
    assert(!pid_sum);

    tsid++;
    uref = uref_block_alloc(uref_mgr, ubuf_mgr,
                            PAT_HEADER_SIZE + PAT_PROGRAM_SIZE + PSI_CRC_SIZE);
    assert(uref != NULL);
    size = -1;
    assert(uref_block_write(uref, 0, &size, &buffer));
    assert(size == PAT_HEADER_SIZE + PAT_PROGRAM_SIZE + PSI_CRC_SIZE);
    pat_init(buffer);
    pat_set_length(buffer, PAT_PROGRAM_SIZE);
    pat_set_tsid(buffer, tsid);
    psi_set_version(buffer, 3);
    psi_set_current(buffer);
    psi_set_section(buffer, 0);
    psi_set_lastsection(buffer, 1);
    pat_program = pat_get_program(buffer, 0);
    patn_init(pat_program);
    patn_set_program(pat_program, 12);
    patn_set_pid(pat_program, 42);
    psi_set_crc(buffer);
    uref_block_unmap(uref, 0, size);
    assert(uref_flow_set_name(uref, "source"));
    assert(upipe_input(upipe_ts_patd, uref));
    assert(!program_sum);
    assert(!pid_sum);

    uref = uref_block_alloc(uref_mgr, ubuf_mgr,
                            PAT_HEADER_SIZE + PAT_PROGRAM_SIZE + PSI_CRC_SIZE);
    assert(uref != NULL);
    size = -1;
    assert(uref_block_write(uref, 0, &size, &buffer));
    assert(size == PAT_HEADER_SIZE + PAT_PROGRAM_SIZE + PSI_CRC_SIZE);
    pat_init(buffer);
    pat_set_length(buffer, PAT_PROGRAM_SIZE);
    pat_set_tsid(buffer, tsid);
    psi_set_version(buffer, 3);
    psi_set_current(buffer);
    psi_set_section(buffer, 1);
    psi_set_lastsection(buffer, 1);
    pat_program = pat_get_program(buffer, 0);
    patn_init(pat_program);
    patn_set_program(pat_program, 12);
    patn_set_pid(pat_program, 43); // invalid: program defined twice
    psi_set_crc(buffer);
    uref_block_unmap(uref, 0, size);
    assert(uref_flow_set_name(uref, "source"));
    assert(upipe_input(upipe_ts_patd, uref));
    assert(!program_sum);
    assert(!pid_sum);

    tsid++;
    uref = uref_block_alloc(uref_mgr, ubuf_mgr,
                            PAT_HEADER_SIZE + PAT_PROGRAM_SIZE + PSI_CRC_SIZE);
    assert(uref != NULL);
    size = -1;
    assert(uref_block_write(uref, 0, &size, &buffer));
    assert(size == PAT_HEADER_SIZE + PAT_PROGRAM_SIZE + PSI_CRC_SIZE);
    pat_init(buffer);
    pat_set_length(buffer, PAT_PROGRAM_SIZE);
    pat_set_tsid(buffer, tsid);
    psi_set_version(buffer, 4);
    psi_set_current(buffer);
    psi_set_section(buffer, 0);
    psi_set_lastsection(buffer, 1);
    pat_program = pat_get_program(buffer, 0);
    patn_init(pat_program);
    patn_set_program(pat_program, 12);
    patn_set_pid(pat_program, 42);
    psi_set_crc(buffer);
    uref_block_unmap(uref, 0, size);
    assert(uref_flow_set_name(uref, "source"));
    assert(upipe_input(upipe_ts_patd, uref));
    assert(!program_sum);
    assert(!pid_sum);

    uref = uref_block_alloc(uref_mgr, ubuf_mgr,
                            PAT_HEADER_SIZE + PAT_PROGRAM_SIZE + PSI_CRC_SIZE);
    assert(uref != NULL);
    size = -1;
    assert(uref_block_write(uref, 0, &size, &buffer));
    assert(size == PAT_HEADER_SIZE + PAT_PROGRAM_SIZE + PSI_CRC_SIZE);
    pat_init(buffer);
    pat_set_length(buffer, PAT_PROGRAM_SIZE);
    pat_set_tsid(buffer, tsid);
    psi_set_version(buffer, 4);
    psi_set_current(buffer);
    psi_set_section(buffer, 1);
    psi_set_lastsection(buffer, 1);
    pat_program = pat_get_program(buffer, 0);
    patn_init(pat_program);
    patn_set_program(pat_program, 13);
    patn_set_pid(pat_program, 43);
    psi_set_crc(buffer);
    uref_block_unmap(uref, 0, size);
    assert(uref_flow_set_name(uref, "source"));
    program_sum = 13; // the first program already exists
    pid_sum = 43;
    assert(upipe_input(upipe_ts_patd, uref));
    assert(!program_sum);
    assert(!pid_sum);

    uref = uref_block_alloc(uref_mgr, ubuf_mgr,
                            PAT_HEADER_SIZE + PAT_PROGRAM_SIZE + PSI_CRC_SIZE);
    assert(uref != NULL);
    size = -1;
    assert(uref_block_write(uref, 0, &size, &buffer));
    assert(size == PAT_HEADER_SIZE + PAT_PROGRAM_SIZE + PSI_CRC_SIZE);
    pat_init(buffer);
    pat_set_length(buffer, PAT_PROGRAM_SIZE);
    pat_set_tsid(buffer, tsid);
    psi_set_version(buffer, 5);
    psi_set_current(buffer);
    psi_set_section(buffer, 0);
    psi_set_lastsection(buffer, 0);
    pat_program = pat_get_program(buffer, 0);
    patn_init(pat_program);
    patn_set_program(pat_program, 13);
    patn_set_pid(pat_program, 43);
    psi_set_crc(buffer);
    uref_block_unmap(uref, 0, size);
    assert(uref_flow_set_name(uref, "source"));
    del_program_sum = 12;
    assert(upipe_input(upipe_ts_patd, uref));
    assert(!del_program_sum);

    uref = uref_block_alloc(uref_mgr, ubuf_mgr,
                            PAT_HEADER_SIZE + PAT_PROGRAM_SIZE * 2 +
                            PSI_CRC_SIZE);
    assert(uref != NULL);
    size = -1;
    assert(uref_block_write(uref, 0, &size, &buffer));
    assert(size == PAT_HEADER_SIZE + PAT_PROGRAM_SIZE * 2 + PSI_CRC_SIZE);
    pat_init(buffer);
    pat_set_length(buffer, PAT_PROGRAM_SIZE * 2);
    pat_set_tsid(buffer, tsid);
    psi_set_version(buffer, 5); // voluntarily set the same version
    psi_set_current(buffer);
    psi_set_section(buffer, 0);
    psi_set_lastsection(buffer, 0);
    pat_program = pat_get_program(buffer, 0);
    patn_init(pat_program);
    patn_set_program(pat_program, 13);
    patn_set_pid(pat_program, 43);
    pat_program = pat_get_program(buffer, 1);
    patn_init(pat_program);
    patn_set_program(pat_program, 14);
    patn_set_pid(pat_program, 44);
    psi_set_crc(buffer);
    uref_block_unmap(uref, 0, size);
    assert(uref_flow_set_name(uref, "source"));
    program_sum = 14;
    pid_sum = 44;
    assert(upipe_input(upipe_ts_patd, uref));
    assert(!del_program_sum);

    upipe_release(upipe_ts_patd);
    upipe_mgr_release(upipe_ts_patd_mgr); // nop

    uref_mgr_release(uref_mgr);
    ubuf_mgr_release(ubuf_mgr);
    udict_mgr_release(udict_mgr);
    umem_mgr_release(umem_mgr);
    uprobe_print_free(uprobe_print);

    return 0;
}