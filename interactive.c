#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "interactive.h"
#include "client.h"
#include "icmp.h"
#include "spdchk.h"

/* ------------------------------------------------------------------ */
/* Session history — volatile dynamic array, freed on exit            */
/* ------------------------------------------------------------------ */

static SessionEntry *history_log  = NULL;
static size_t        log_capacity = 10;
static size_t        log_size     = 0;

static int history_init(void)
{
    history_log = malloc(log_capacity * sizeof(SessionEntry));
    return history_log ? 0 : -1;
}

static void history_free(void)
{
    free(history_log);
    history_log  = NULL;
    log_size     = 0;
    log_capacity = 0;
}

static int history_append(const SessionEntry *e)
{
    if (log_size == log_capacity) {
        size_t        new_cap = log_capacity * 2;
        SessionEntry *tmp     = realloc(history_log,
                                        new_cap * sizeof(SessionEntry));
        if (!tmp)
            return -1;
        history_log  = tmp;
        log_capacity = new_cap;
    }
    history_log[log_size++] = *e;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Misc helpers                                                        */
/* ------------------------------------------------------------------ */

static void get_timestamp(char *buf, size_t len)
{
    time_t    now     = time(NULL);
    struct tm *tm_inf = localtime(&now);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", tm_inf);
}

/* Drain any leftover characters up to and including the newline. */
static void flush_input(void)
{
    int c;
    while ((c = getchar()) != '\n' && c != EOF)
        ;
}

/* ------------------------------------------------------------------ */
/* Menu helpers                                                        */
/* ------------------------------------------------------------------ */

static void print_sep(void)
{
    printf("--------------------------------------------------\n");
}

static void print_main_menu(int streams, int duration, int ping_count)
{
    print_sep();
    printf("  spdchk %s — Interactive Mode\n", SPDCHK_VERSION);
    print_sep();
    printf("  1. Run Reachability (ICMP)  [pings: %d]\n", ping_count);
    printf("  2. Run Bandwidth   (TCP)    [streams: %d, duration: %d s]\n",
           streams, duration);
    printf("  3. View Session History     [%zu test(s) performed]\n", log_size);
    printf("  4. Change Parameters\n");
    printf("  5. Exit\n");
    print_sep();
    printf("  Selection: ");
    fflush(stdout);
}

static void print_params_menu(int streams, int duration, int ping_count)
{
    print_sep();
    printf("  Change Parameters\n");
    print_sep();
    printf("  1. TCP streams     (current: %d)\n",   streams);
    printf("  2. TCP duration    (current: %d s)\n", duration);
    printf("  3. ICMP ping count (current: %d)\n",   ping_count);
    printf("  4. Back\n");
    print_sep();
    printf("  Selection: ");
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* State: display latest ICMP results                                  */
/* ------------------------------------------------------------------ */

static void state_results_icmp(const struct icmp_stats *s, int rc)
{
    print_sep();
    printf("  ICMP Results\n");
    print_sep();
    if (rc == 0) {
        printf("  Avg RTT:     %.2f ms\n", s->avg_latency_ms);
        printf("  Packet loss: %.1f%%\n",  s->packet_loss_pct);
    } else {
        printf("  Target unreachable — all pings lost.\n");
    }
    print_sep();
}

/* ------------------------------------------------------------------ */
/* State: display latest bandwidth results                             */
/* ------------------------------------------------------------------ */

static void state_results_bw(const struct run_client_result *r,
                              int streams, int duration)
{
    print_sep();
    printf("  Bandwidth Results\n");
    print_sep();
    printf("  Throughput:  %.2f Gbps\n", r->throughput_gbps);
    printf("  Avg RTT:     %.2f ms\n",   r->avg_latency_ms);
    printf("  Packet loss: %.1f%%\n",    r->packet_loss_pct);
    printf("  Streams:     %d\n",        streams);
    printf("  Duration:    %d s\n",      duration);
    print_sep();
}

/* ------------------------------------------------------------------ */
/* State: tabulate session history                                     */
/* ------------------------------------------------------------------ */

static void state_history(void)
{
    print_sep();
    printf("  Session History\n");
    print_sep();

    if (log_size == 0) {
        printf("  No tests recorded yet.\n");
    } else {
        printf("  %-3s  %-19s  %-8s  %-10s  %-13s  %s\n",
               "#", "Timestamp", "Streams", "Duration", "Throughput", "RTT");
        printf("  %-3s  %-19s  %-8s  %-10s  %-13s  %s\n",
               "---", "-------------------", "-------",
               "---------", "------------", "------");
        for (size_t i = 0; i < log_size; i++) {
            const SessionEntry *e = &history_log[i];
            if (e->is_icmp_only) {
                printf("  %-3zu  %-19s  %-8s  %-10s  %-13s  %.2f ms\n",
                       i + 1, e->timestamp, "ICMP", "-", "-", e->rtt_ms);
            } else {
                printf("  %-3zu  %-19s  %-8u  %-9us  %-10.2f Gbps  %.2f ms\n",
                       i + 1, e->timestamp,
                       e->streams, e->duration_sec,
                       e->throughput_gbps, e->rtt_ms);
            }
        }
    }

    print_sep();
    printf("  Press ENTER to return...");
    fflush(stdout);
    flush_input();
}

/* ------------------------------------------------------------------ */
/* State: change parameters sub-menu                                   */
/* ------------------------------------------------------------------ */

static void state_change_params(int *streams, int *duration, int *ping_count)
{
    int choice;
    do {
        print_params_menu(*streams, *duration, *ping_count);
        if (scanf("%d", &choice) != 1) {
            flush_input();
            continue;
        }
        flush_input();

        int val;
        switch (choice) {
        case 1:
            printf("  New stream count (1-%d): ", DSS_MAX_STREAMS);
            fflush(stdout);
            if (scanf("%d", &val) == 1 && val >= 1 && val <= DSS_MAX_STREAMS)
                *streams = val;
            else
                printf("  Invalid value — unchanged.\n");
            flush_input();
            break;
        case 2:
            printf("  New duration (1-3600 s): ");
            fflush(stdout);
            if (scanf("%d", &val) == 1 && val >= 1 && val <= 3600)
                *duration = val;
            else
                printf("  Invalid value — unchanged.\n");
            flush_input();
            break;
        case 3:
            printf("  New ICMP ping count (1-100): ");
            fflush(stdout);
            if (scanf("%d", &val) == 1 && val >= 1 && val <= 100)
                *ping_count = val;
            else
                printf("  Invalid value — unchanged.\n");
            flush_input();
            break;
        case 4:
            break;
        default:
            printf("  Invalid choice — enter 1-4.\n");
        }
    } while (choice != 4);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int interactive_main(const char *target_ip, int port, int ping_count)
{
    int streams  = DEFAULT_STREAMS;
    int duration = DEFAULT_DURATION;

    if (history_init() != 0) {
        fprintf(stderr, "interactive: out of memory\n");
        return -1;
    }

    printf("\nWelcome to spdchk %s — Interactive Mode\n", SPDCHK_VERSION);
    printf("Server: %s:%d\n\n", target_ip, port);

    int running = 1;
    while (running) {
        print_main_menu(streams, duration, ping_count);

        int choice;
        if (scanf("%d", &choice) != 1) {
            flush_input();
            printf("\n");
            continue;
        }
        flush_input();
        printf("\n");

        switch (choice) {

        /* -------------------------------------------------------- */
        /* 1. ICMP Reachability                                      */
        /* -------------------------------------------------------- */
        case 1: {
            printf("  Running ICMP ping to %s (%d pings)...\n",
                   target_ip, ping_count);
            struct icmp_stats s = { 0 };
            int rc = icmp_ping(target_ip, ping_count, &s);
            state_results_icmp(&s, rc);

            SessionEntry e = {
                .streams         = 0,
                .duration_sec    = 0,
                .throughput_gbps = 0.0,
                .rtt_ms          = (rc == 0) ? s.avg_latency_ms : -1.0,
                .is_icmp_only    = 1,
            };
            get_timestamp(e.timestamp, sizeof(e.timestamp));
            history_append(&e);

            printf("  Press ENTER to continue...");
            fflush(stdout);
            flush_input();
            break;
        }

        /* -------------------------------------------------------- */
        /* 2. TCP Bandwidth                                          */
        /* -------------------------------------------------------- */
        case 2: {
            printf("  Running bandwidth test to %s (%d streams, %d s)...\n",
                   target_ip, streams, duration);

            struct client_args args = {
                .target_ip     = target_ip,
                .port          = port,
                .ping_count    = ping_count,
                .duration      = duration,
                .streams       = streams,
                .json_output   = 0,
                .output_path   = NULL,
                .dss_mode      = 0,
                .dss_window_ms = DSS_WINDOW_MS,
            };

            struct run_client_result result = { 0 };
            int rc = run_client_ex(&args, &result);

            if (rc == 0) {
                state_results_bw(&result, streams, duration);

                SessionEntry e = {
                    .streams         = (uint32_t)streams,
                    .duration_sec    = (uint32_t)duration,
                    .throughput_gbps = result.throughput_gbps,
                    .rtt_ms          = result.avg_latency_ms,
                    .is_icmp_only    = 0,
                };
                get_timestamp(e.timestamp, sizeof(e.timestamp));
                history_append(&e);
            } else {
                printf("  ERROR: bandwidth test failed.\n");
            }

            printf("  Press ENTER to continue...");
            fflush(stdout);
            flush_input();
            break;
        }

        /* -------------------------------------------------------- */
        /* 3. View Session History                                   */
        /* -------------------------------------------------------- */
        case 3:
            state_history();
            break;

        /* -------------------------------------------------------- */
        /* 4. Change Parameters                                      */
        /* -------------------------------------------------------- */
        case 4:
            state_change_params(&streams, &duration, &ping_count);
            break;

        /* -------------------------------------------------------- */
        /* 5. Exit                                                   */
        /* -------------------------------------------------------- */
        case 5:
            running = 0;
            break;

        default:
            printf("  Invalid choice — enter 1-5.\n");
        }

        printf("\n");
    }

    printf("Exiting interactive mode. Session history discarded.\n");
    history_free();
    return 0;
}
