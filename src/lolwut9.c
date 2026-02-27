/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "server.h"
#include "lolwut.h"

/* ASCII characters used to represent the number of iterations in the Julia set. */
static char ascii_array[] = " .:-=+*%#&@";

/* The Julia set is defined as the set of points in the complex plane that
 * do not escape to infinity under repeated iteration of the function
 * f(z) = z^2 + c, where c is a complex constant. We can visualize this
 * set by mapping the number of iterations it takes for each point to escape
 * to a character with increasing density. */
static inline int juliaSetIteration(float x, float y, float julia_r, float julia_i, int max_iter) {
    for (int i = 0; i < max_iter; i++) {
        float x_new = x * x - y * y + julia_r;
        float y_new = 2 * x * y + julia_i;
        x = x_new;
        y = y_new;
        if (x * x + y * y > 4) return i;
    }
    return max_iter - 1;
}

/* The LOLWUT 9 command:
 *
 * LOLWUT [columns rows] [real imaginary]
 *
 * By default the command uses 80 columns, 40 squares per row per column with
 * random constants for the Julia set formula.
 */
void lolwut9Command(client *c) {
    long cols = 80;
    long rows = 40;
    float julia_r, julia_i;

    if (c->argc == 2 || c->argc == 4 || c->argc > 5) {
        addReplyError(c, "Syntax error. Use: LOLWUT [columns rows] [real imaginary]");
        return;
    }

    if (c->argc > 1 && getLongFromObjectOrReply(c, c->argv[1], &cols, NULL) != C_OK) return;
    if (c->argc > 2 && getLongFromObjectOrReply(c, c->argv[2], &rows, NULL) != C_OK) return;

    /* Limits. We want LOLWUT to be always reasonably fast and cheap to execute
     * so we have maximum number of columns, rows, and output resolution. */
    if (cols < 1) cols = 1;
    if (cols > 160) cols = 160;
    if (rows < 1) rows = 1;
    if (rows > 80) rows = 80;

    if (c->argc == 5) {
        long double input_r, input_i;
        if (getLongDoubleFromObjectOrReply(c, c->argv[3], &input_r, NULL) != C_OK) return;
        if (getLongDoubleFromObjectOrReply(c, c->argv[4], &input_i, NULL) != C_OK) return;
        julia_r = input_r;
        julia_i = input_i;
    } else {
        /* Use random values if the user didn't specify constants. */
        julia_r = rand() / (float)RAND_MAX * 2 - 1;
        julia_i = rand() / (float)RAND_MAX * 2 - 1;
    }

    sds output_array = sdsnewlen(NULL, sizeof(char) * (cols + 1) * rows);
    for (int i = 0; i < rows; i++) {
        for (int j = 0; j < cols; j++) {
            /* Center x and y to middle of the cell. */
            float x = -2.0f + 4.0f * (j + 0.5f) / cols;
            float y = 2.0f - 4.0f * (i + 0.5f) / rows;

            int iterations = juliaSetIteration(x, y, julia_r, julia_i, sizeof(ascii_array) - 1);
            output_array[i * (cols + 1) + j] = ascii_array[iterations % sizeof(ascii_array)];
        }
        output_array[i * (cols + 1) + cols] = '\n';
    }
    output_array = sdscatprintf(output_array, "Ascii representation of Julia set with constant %.2f + %.2fi\n", julia_r, julia_i);
    output_array = sdscatprintf(output_array, "Don't forget to have fun! %s ver. ", server.extended_redis_compat ? "Redis" : "Valkey");
    output_array = sdscat(output_array, server.extended_redis_compat ? REDIS_VERSION : VALKEY_VERSION);
    output_array = sdscatlen(output_array, "\n", 1);
    addReplyVerbatim(c, output_array, sdslen(output_array), "txt");
    sdsfree(output_array);
}
