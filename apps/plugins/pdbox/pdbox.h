/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2009 Wincent Balin
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#ifndef PDBOX_H
#define PDBOX_H

/* Use dbestfit. */
#include "bmalloc.h"
#include "dmalloc.h"

/* Pure Data */
#include "m_pd.h"


/* dbestfit declarations. */

/* Minimal memory size. */
#define MIN_MEM_SIZE (4 * 1024 * 1024)


/* Audio declarations. */
#define PD_SAMPLERATE 32000
#define PD_SAMPLES_QUOT (PD_SAMPLERATE / HZ)
#define PD_SAMPLES_REM  (PD_SAMPLERATE % HZ)
#if PD_SAMPLES_REM == 0
  #define PD_SAMPLES_PER_HZ (PD_SAMPLES_QUOT)
#else
  #define PD_SAMPLES_PER_HZ (PD_SAMPLES_QUOT + 1)
#endif

/* Audio data types. */
#define PD_AUDIO_BLOCK_SIZE PD_SAMPLES_PER_HZ
struct pdbox_audio_block
{
    unsigned int fill;
    int32_t data[PD_AUDIO_BLOCK_SIZE];
};


/* Additional functions. */
char *rb_strncat(char *s, const char *t, size_t n);
double rb_strtod(const char*, char**);
double rb_atof(const char*);
void rb_ftoan(float, char*, int);
float rb_floor(float);
long rb_atol(const char* s);
float rb_sin(float rad);
float rb_cos(float rad);
int rb_fscanf_f(int fd, float* f);
int rb_fprintf_f(int fd, float f);
float rb_log10(float);
float rb_log(float);
float rb_exp(float);
float rb_pow(float, float);
float rb_sqrt(float);
float rb_fabs(float);
float rb_atan(float);
float rb_atan2(float, float);
float rb_sinh(float);
float rb_tan(float);
typedef struct
{
    int quot;
    int rem;
}
div_t;
div_t div(int x, int y);
void sys_findlibdir(const char* filename);
int sys_startgui(const char *guidir);


/* Network declarations. */

/* Maximal size of the datagram. */
#define MAX_DATAGRAM_SIZE 255

/* This structure replaces a UDP datagram. */
struct datagram
{
    bool used;
    uint8_t size;
    char data[MAX_DATAGRAM_SIZE];
};

/* Prototypes of network functions. */
void net_init(void);
void net_destroy(void);
bool send_datagram(struct event_queue* route, int port,
                   char* data, size_t size);
bool receive_datagram(struct event_queue* route, int port,
                      struct datagram* buffer);

/* Network message queues. */
extern struct event_queue gui_to_core;
extern struct event_queue core_to_gui;

/* UDP ports of the original software. */
#define PD_CORE_PORT 3333
#define PD_GUI_PORT 3334

/* Convinience macros. */
#define SEND_TO_CORE(data) \
    send_datagram(&gui_to_core, PD_CORE_PORT, data, strlen(data))
#define RECEIVE_TO_CORE(buffer) \
    receive_datagram(&gui_to_core, PD_CORE_PORT, buffer)
#define SEND_FROM_CORE(data) \
    send_datagram(&core_to_gui, PD_GUI_PORT, data, strlen(data))
#define RECEIVE_FROM_CORE(buffer) \
    receive_datagram(&core_to_gui, PD_GUI_PORT, buffer)

/* PD core message callback. */
void rockbox_receive_callback(struct datagram* dg);


/* Pure Data declarations. */

/* Pure Data function prototypes. */
void pd_init(void);


/* Redefinitons of ANSI C functions. */
#include "lib/wrappers.h"

#define strncat rb_strncat
#define floor rb_floor
#define atof rb_atof
#define atol rb_atol
#define ftoan rb_ftoan
#define sin rb_sin
#define cos rb_cos
#define log10 rb_log10
#define log rb_log
#define exp rb_exp
#define pow rb_pow
#define sqrt rb_sqrt
#define fabs rb_fabs
#define atan rb_atan
#define atan2 rb_atan2
#define sinh rb_sinh
#define tan rb_tan

#endif
