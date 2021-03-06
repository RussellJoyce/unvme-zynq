/**
 * @file
 * @brief Device read/write utility.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <ctype.h>
#include <err.h>

#include "unvme.h"

#define PDEBUG(fmt, arg...)     //fprintf(stderr, fmt "\n", ##arg)

// Global static variables
static const unvme_ns_t* ns;    ///< namespace handle
static u32 rw = 0;              ///< read-write flag
static u64 startlba = 0;        ///< starting LBA
static u64 lbacount = 0;        ///< LBA count
static u64 pattern = 0;         ///< 64-bit data pattern
static u64 patinc = 0;          ///< pattern increment per LBA
static u32 qcount = 16;         ///< IO queue count
static u32 qdepth = 64;         ///< IO queue depth
static u32 nbpio = 0;           ///< number of blocks per IO
static time_t dumptime = 0;     ///< interval to display data
static int dump = 0;            ///< dump count
static int mismatch = 0;        ///< data miscompare flag
static unvme_iod_t* iods;       ///< array IO descriptors
static void** iobufs;           ///< array of IO buffers
static u64* fixedbuf;           ///< fixed data block buffer


/*
 * Dump buffer content in hex.
 */
static void dumpblock(void* buf, u64 lba)
{
    printf("===== LBA 0x%lx =====\n", lba);
    u64* p = buf;
    u64 w0 = ~*p, w1 = 0, w2 = 0, w3 = 0;
    int i, len = ns->blocksize, skip = 0;
    for (i = 0; i < len; i += 32) {
        if (p[0] != w0 || p[1] != w1 || p[2] != w2 || p[3] != w3) {
            printf("%04x: %016lx %016lx %016lx %016lx\n",
                   i, p[0], p[1], p[2], p[3]);
            skip = 0;
        } else if (!skip) {
            printf("*\n");
            skip = 1;
        }
        w0 = p[0];
        w1 = p[1];
        w2 = p[2];
        w3 = p[3];
        p += 4;
    }
}

/*
 * Submit next IO to a queue.
 */
unvme_iod_t submit(int q, int d, void* buf, u64 lba, u32 nlb)
{
    unvme_iod_t iod;

    if (rw == 'w') {
        int b, i;
        if (patinc) {
            u64* pbuf = buf;
            int wib = ns->blocksize / sizeof(u64);
            for (b = 0; b < nlb; b++) {
                u64 p = pattern + ((b + lba - startlba) * patinc);
                for (i = 0; i < wib; i++) *pbuf++ = p;
            }
        }
        if (dump) {
            void* bbuf = buf;
            for (b = 0; b < nlb; b++) {
                dumpblock(bbuf, lba + b);
                bbuf += ns->blocksize;
                if (--dump == 0) break;
            }
        }
        PDEBUG("@W q%d.%d %p %#lx %d", q, d, buf, lba, nlb);
        iod = unvme_awrite(ns, q, buf, lba, nlb);
        if (!iod) errx(1, "unvme_awrite q=%d lba=%#lx nlb=%#x failed", q, lba, nlb);
    } else {
        PDEBUG("@R q%d.%d %p %#lx %d", q,  d, buf, lba, nlb);
        iod = unvme_aread(ns, q, buf, lba, nlb);
        if (!iod) errx(1, "unvme_aread q=%d lba=%#lx nlb=%#x failed", q, lba, nlb);
    }
    return iod;
}

/*
 * Main.
 */
int main(int argc, char** argv)
{
    // parsing command options
    const char* usage = "Usage: %s [OPTION]... PCINAME\n\
         -w PATTERN   write the specified (64-bit) data pattern\n\
         -r PATTERN   read and compare against the specified data pattern\n\
         -i PATINC    increment data pattern at each LBA (default 0)\n\
         -a LBA       starting at LBA (default 0)\n\
         -n COUNT     number of blocks to read/write (default to end)\n\
         -q QCOUNT    use number of queues for async IO (default 16)\n\
         -d QDEPTH    use queue depth for async IO (default 64)\n\
         -m NBPIO     use number of blocks per IO (default max support)\n\
         -p INTERVAL  print progress with LBA data every INTERVAL seconds\n\
         PCINAME      PCI device name (as 01:00.0[/1] format)\n\n\
         either -w or -r must be specified";

    const char* prog = strrchr(argv[0], '/');
    prog = prog ? prog + 1 : argv[0];
    int opt, b, i;

    while ((opt = getopt(argc, argv, "w:r:i:a:n:q:d:m:p:")) != -1) {
        switch (opt) {
        case 'w':
        case 'r':
            rw = opt;
            pattern = strtoull(optarg, 0, 0);
            break;
        case 'i':
            patinc = strtoull(optarg, 0, 0);
            break;
        case 'a':
            startlba = strtoull(optarg, 0, 0);
            break;
        case 'n':
            lbacount = strtoull(optarg, 0, 0);
            break;
        case 'q':
            qcount = strtoul(optarg, 0, 0);
            break;
        case 'd':
            qdepth = strtoul(optarg, 0, 0);
            break;
        case 'm':
            nbpio = strtoul(optarg, 0, 0);
            break;
        case 'p':
            dumptime = strtoul(optarg, 0, 0);
            dump = 2;
            break;
        default:
            warnx(usage, prog);
            exit(1);
        }
    }
    if ((optind + 1) != argc || !rw) {
        warnx(usage, prog);
        exit(1);
    }
    char* pciname = argv[optind];

    // open device and allocate buffer
    time_t tstart = time(0);
    ns = unvme_open(pciname);
    if (!ns) exit(1);
    if ((startlba + lbacount) > ns->blockcount) {
        unvme_close(ns);
        errx(1, "max block count is %#lx", ns->blockcount);
    }
    if (qcount > ns->qcount || qdepth >= ns->qsize) {
        unvme_close(ns);
        errx(1, "max qcount=%d qdepth=%d", ns->qcount, ns->qsize-1);
    }
    if (lbacount == 0) lbacount = ns->blockcount - startlba;
    if (nbpio == 0) nbpio = ns->maxbpio;
    if (nbpio > ns->maxbpio || (nbpio % ns->nbpp)) {
        unvme_close(ns);
        errx(1, "invalid nbpio %d", nbpio);
    }

    printf("%s qc=%d/%d qd=%d/%d bc=%#lx bs=%d nbpio=%d/%d\n",
            ns->device, qcount, ns->qcount, qdepth, ns->qsize-1,
            ns->blockcount, ns->blocksize, nbpio, ns->maxbpio);

    int iomax = qcount * qdepth;
    iods = calloc(iomax, sizeof(unvme_iod_t));
    iobufs = calloc(iomax, sizeof(void*));

    int iobufsize = nbpio * ns->blocksize;
    for (i = 0; i < iomax; i++) {
        iobufs[i] = unvme_alloc(ns, iobufsize);
        if (!iobufs[i]) errx(1, "unvme_alloc %#x failed", iobufsize);
    }

    int wib = ns->blocksize / sizeof(u64);
    if (patinc == 0) {
        fixedbuf = malloc(ns->blocksize);
        for (i = 0; i < wib; i++) fixedbuf[i] = pattern;
    }

    // setup for write and read
    if (rw == 'w') {
        printf("WRITE lba=%#lx-%#lx pat=%#lx inc=%#lx\n",
               startlba, startlba + lbacount - 1, pattern, patinc);

        // if fixed pattern then fill all buffers with the pattern
        if (patinc == 0) {
            for (i = 0; i < iomax; i++) {
                void* buf = iobufs[i];
                for (b = 0; b < nbpio; b++) {
                    memcpy(buf, fixedbuf, ns->blocksize);
                    buf += ns->blocksize;
                }
            }
        }
    } else {
        printf("READ lba=%#lx-%#lx pat=%#lx inc=%#lx\n",
               startlba, startlba + lbacount - 1, pattern, patinc);
    }

    // submit async IOs until all are completed
    u64 submitcount = lbacount;
    u64 completecount = lbacount;
    u64 nextlba = startlba;
    time_t tio = time(0);
    time_t dumplast = tio;
    int q = 0, d = 0;

    while (completecount > 0) {
        int x = q * qdepth + d;
        unvme_iod_t iod = iods[x];

        // check to submit next IO
        if (!iod) {
            if (submitcount > 0) {
                if (!mismatch) {
                    int nlb = nbpio;
                    if (nlb > submitcount) nlb = submitcount;
                    iods[x] = submit(q, d, iobufs[x], nextlba, nlb);
                    nextlba += nlb;
                    submitcount -= nlb;
                } else {
                    completecount -= submitcount;
                    submitcount = 0;
                }
            }

            // next slot
            if (++d >= qdepth) {
                d = 0;
                if (++q >= qcount) q = 0;
            }
            continue;
        }

        // save iod content as unvme_apoll() will clear it on completion
        void* cbuf = iod->buf;
        u64 clba = iod->slba;
        u32 cnlb = iod->nlb;

        // check IO completion
        int stat = unvme_apoll(iod, 0);
        if (stat) {
            // terminate on error
            if (stat != -1)
                errx(1, "unvme_apoll error=%#x slba=%#lx nlb=%#x", stat, iod->slba, iod->nlb);
            else if ((time(0) - tio) > UNVME_TIMEOUT)
                errx(1, "unvme_apoll timeout slba=%#lx nlb=%#x", iod->slba, iod->nlb);
            // if no completion go on to next queue
            if (++q >= qcount) q = 0;
            continue;
        }

        // IO completion
        PDEBUG("@C q%d.%d %p %#lx %d", q, d, cbuf, clba, cnlb);
        completecount -= cnlb;
        iods[x] = NULL;
        tio = time(0);
        if (dumptime && (tio - dumplast) > dumptime) {
            dumplast = tio;
            dump++;
        }

        // compare read result unless (there's already a data mismatch)
        if (rw == 'r' && !mismatch) {
            // print block contents
            if (dump) {
                void* bbuf = cbuf;
                for (b = 0; b < cnlb; b++) {
                    dumpblock(bbuf, clba + b);
                    bbuf += ns->blocksize;
                    if (--dump == 0) break;
                }
            }

            // compare read results against data pattern
            if (patinc) {
                void* bbuf = cbuf;
                u64 blba = clba;
                for (b = 0; b < cnlb; b++) {
                    u64 p = pattern + ((blba - startlba) * patinc);
                    u64* pbuf = bbuf;
                    for (i = 0; i < wib; i++) {
                        if (*pbuf != p) {
                            dumpblock(bbuf, blba);
                            warnx("ERROR: data mismatch at LBA %#lx "
                                    "offset %#lx exp %#016lx obs %#016lx",
                                    blba, i * sizeof(u64), p, *pbuf);
                            mismatch++;
                            break;
                        }
                        pbuf++;
                    }
                    if (mismatch) break;
                    bbuf += ns->blocksize;
                    blba++;
                }
            } else {
                void* bbuf = cbuf;
                u64 plba = clba;
                for (b = 0; b < cnlb; b++) {
                    if (memcmp(bbuf, fixedbuf, ns->blocksize)) {
                        dumpblock(bbuf, plba);
                        warnx("ERROR: data mismatch at LBA %#lx exp %#016lx",
                                plba, pattern);
                        mismatch++;
                        break;
                    }
                    bbuf += ns->blocksize;
                    plba++;
                }
            }
        }
    }

    for (i = 0; i < iomax; i++) unvme_free(ns, iobufs[i]);
    free(fixedbuf);
    free(iobufs);
    free(iods);
    unvme_close(ns);

    if (!mismatch) printf("Completion time: %ld seconds\n", time(0) - tstart);

    return mismatch;
}

