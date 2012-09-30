/*
 * Copyright 2011-2012 Samy Al Bahra.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include <ck_ring.h>
#include "../../common.h"

#ifndef ITERATIONS
#define ITERATIONS 128
#endif

struct context {
	unsigned int tid;
	unsigned int previous;
	unsigned int next;
};

struct entry {
	unsigned int magic;
	unsigned int ref;
	int tid;
	int value;
};

static int nthr;
static ck_ring_t *ring;
static ck_ring_t ring_mpmc;
static struct affinity a;
static int size;
static volatile int barrier;
static int eb;

static void *
test_mpmc(void *c)
{
	struct entry *entry;
	unsigned int observed = 0, foreign = 0;
	int i, j;

	(void)c;
        if (aff_iterate(&a)) {
                perror("ERROR: Could not affine thread");
                exit(EXIT_FAILURE);
        }

	ck_pr_inc_int(&eb);
	while (ck_pr_load_int(&eb) != nthr);

	for (i = 0; i < ITERATIONS; i++) {
		for (j = 0; j < size; j++) {
			struct entry *o;

			entry = malloc(sizeof(*entry));
			assert(entry != NULL);
			entry->magic = 0xdead;
			entry->tid = j;
			entry->value = j;
			entry->ref = 0;

			if (ck_ring_enqueue_mpmc(&ring_mpmc, entry) == false) {
				free(entry);
				j--;
				if (j < 0)
					j = 0;
			}

			/* Keep trying until we encounter at least one node. */
			if (ck_ring_dequeue_mpmc(&ring_mpmc, &o) == false) {
				j--;
				if (j < 0)
					j = 0;
				
				continue;
			}

			observed++;
			foreign += entry != o;
			if (o->value < 0 || o->value >= size || o->value != o->tid || o->magic != 0xdead) {
				fprintf(stderr, "[%p] (%x) (%d, %d) >< (0, %d)\n",
					(void *)o, o->magic, o->tid, o->value, size);
				exit(EXIT_FAILURE);
			}

			o->magic = 0xbeef;
			o->value = -31337;
			o->tid = -31338;

			if (ck_pr_faa_uint(&o->ref, 1) != 0) {
				fprintf(stderr, "[%p] We dequeued twice.\n", (void *)o);
				exit(EXIT_FAILURE);
			}

			free(o);
		}
	}

	fprintf(stderr, "Observed %u / Foreign %u\n", observed, foreign);
	return NULL;
}

static void *
test(void *c)
{
	struct context *context = c;
	struct entry *entry;
	int i, j;
	bool r;

        if (aff_iterate(&a)) {
                perror("ERROR: Could not affine thread");
                exit(EXIT_FAILURE);
        }

	if (context->tid == 0) {
		struct entry *entries;

		entries = malloc(sizeof(struct entry) * size);
		assert(entries != NULL);

		if (ck_ring_size(ring) != 0) {
			fprintf(stderr, "More entries than expected: %u > 0\n",
				ck_ring_size(ring));
			exit(EXIT_FAILURE);
		}

		for (i = 0; i < size; i++) {
			entries[i].value = i;
			entries[i].tid = 0;

			r = ck_ring_enqueue_mpmc(ring, entries + i);
			assert(r != false);
		}

		if (ck_ring_size(ring) != (unsigned int)size) {
			fprintf(stderr, "Less entries than expected: %u < %d\n",
				ck_ring_size(ring), size);
			exit(EXIT_FAILURE);
		}

		if (ck_ring_capacity(ring) != ck_ring_size(ring) + 1) {
			fprintf(stderr, "Capacity less than expected: %u < %u\n",
				ck_ring_size(ring), ck_ring_capacity(ring));
			exit(EXIT_FAILURE);
		}

		barrier = 1;
	}

	while (barrier == 0);

	for (i = 0; i < ITERATIONS; i++) {
		for (j = 0; j < size; j++) {
			while (ck_ring_dequeue_mpmc(ring + context->previous, &entry) == false);

			if (context->previous != (unsigned int)entry->tid) {
				fprintf(stderr, "[%u:%p] %u != %u\n",
					context->tid, (void *)entry, entry->tid, context->previous);
				exit(EXIT_FAILURE);
			}

			if (entry->value < 0 || entry->value >= size) {
				fprintf(stderr, "[%u:%p] %u </> %u\n",
					context->tid, (void *)entry, entry->tid, context->previous);
				exit(EXIT_FAILURE);
			}

			entry->tid = context->tid;
			r = ck_ring_enqueue_mpmc(ring + context->tid, entry);
			assert(r == true);
		}
	}

	return NULL;
}

int
main(int argc, char *argv[])
{
	int i, r;
	void *buffer;
	struct context *context;
	pthread_t *thread;

	if (argc != 4) {
		fprintf(stderr, "Usage: validate <threads> <affinity delta> <size>\n");
		exit(EXIT_FAILURE);
	}

	a.request = 0;
	a.delta = atoi(argv[2]);

	nthr = atoi(argv[1]);
	assert(nthr >= 1);

	size = atoi(argv[3]);
	assert(size > 4 && (size & size - 1) == 0);
	size -= 1;

	ring = malloc(sizeof(ck_ring_t) * nthr);
	assert(ring);

	context = malloc(sizeof(*context) * nthr);
	assert(context);

	thread = malloc(sizeof(pthread_t) * nthr);
	assert(thread);

	fprintf(stderr, "SPSC test:");
	for (i = 0; i < nthr; i++) {
		context[i].tid = i;
		if (i == 0) {
			context[i].previous = nthr - 1;
			context[i].next = i + 1;
		} else if (i == nthr - 1) {
			context[i].next = 0;
			context[i].previous = i - 1;
		} else {
			context[i].next = i + 1;
			context[i].previous = i - 1;
		}

		buffer = malloc(sizeof(void *) * (size + 1));
		assert(buffer);
		memset(buffer, 0, sizeof(void *) * (size + 1));
		ck_ring_init(ring + i, buffer, size + 1);
		r = pthread_create(thread + i, NULL, test, context + i);
		assert(r == 0);
	}

	for (i = 0; i < nthr; i++)
		pthread_join(thread[i], NULL);
	fprintf(stderr, " done\n");

	fprintf(stderr, "MPMC test:\n");
	buffer = malloc(sizeof(void *) * (size + 1));
	assert(buffer);
	memset(buffer, 0, sizeof(void *) * (size + 1));
	ck_ring_init(&ring_mpmc, buffer, size + 1);
	for (i = 0; i < nthr; i++) {
		r = pthread_create(thread + i, NULL, test_mpmc, context + i);
		assert(r == 0);
	}

	for (i = 0; i < nthr; i++)
		pthread_join(thread[i], NULL);
	return (0);
}

