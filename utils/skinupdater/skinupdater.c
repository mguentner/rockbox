/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id: tag_table.c 26346 2010-05-28 02:30:27Z jdgordon $
 *
 * Copyright (C) 2010 Jonathan Gordon
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

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include "tag_table.h"

#define PUTCH(out, c) fprintf(out, "%c", c)
extern struct tag_info legal_tags[];


/** Command line setting **/
bool is_mono_display = false;




/* dump "count" args to output replacing '|' with ',' except after the last count.
 * return the amount of chars read. (start+return will be after the last | )
 */
int dump_arg(FILE* out, const char* start, int count, bool close)
{
    int l = 0;
    while (count)
    {
        if (start[l] == '|')
        {
            if (count > 1)
            {
                PUTCH(out, ',');
            } else if (close) {
                PUTCH(out, ')');
            }
            count--;
        } else {
            PUTCH(out, start[l]);
        }
        l++;
    }
    return l;
}

#define MATCH(s) (!strcmp(tag->name, s))
int parse_tag(FILE* out, const char* start, bool in_conditional)
{
    struct tag_info *tag;
    int len = 0;
    for(tag = legal_tags; 
        tag->name[0] && strncmp(start, tag->name, strlen(tag->name)) != 0;
        tag++) ;
    if (!tag->name[0])
        return -1;
    if (tag->params[0] == '\0')
    {
        fprintf(out, "%s", tag->name);
        return strlen(tag->name);
    }
    fprintf(out, "%s", tag->name);
    len += strlen(tag->name);
    start += len;
    /* handle individual tags which accept params */
    if ((MATCH("bl") || MATCH("pb") || MATCH("pv")) && !in_conditional)
    {
        if (*start == '|')
        {
            len++; start++;
            PUTCH(out, '(');
            /* TODO: need to verify that we are actually using the long form... */
            len += dump_arg(out, start, 5, true);
        }
    }
    else if (MATCH("d") || MATCH("D") || MATCH("mv") || MATCH("pS") || MATCH("pE") || MATCH("t") || MATCH("Tl"))
    {
        char temp[8] = {'\0'};
        int i = 0;
        /* tags which maybe have a number after them */
        while ((*start >= '0' && *start <= '9') || *start == '.')
        {
            temp[i++] = *start++;
        }
        if (i!= 0)
        {
            fprintf(out, "(%s)", temp);
            len += i;
        }
    }
    else if (MATCH("xl"))
    {
        PUTCH(out, '(');
        int read = 1+dump_arg(out, start+1, 4, false);
        len += read;
        start += read;
        if (*start>= '0' && *start <= '9')
        {
            PUTCH(out, ',');
            len += dump_arg(out, start, 1, false);
        }
        PUTCH(out, ')');
    }
    else if (MATCH("xd"))
    {
        /* NOTE: almost certainly needs work */
        PUTCH(out, '(');
        PUTCH(out, *start++); len++;
        if ((*start >= 'a' && *start <= 'z') ||
            (*start >= 'A' && *start <= 'Z'))
        {
            PUTCH(out, *start); len++;
        }
        PUTCH(out, ')');
    }
    else if (MATCH("x"))
    {
        PUTCH(out, '(');
        len += 1+dump_arg(out, start+1, 4, true);
    }
    else if (MATCH("Fl"))
    {
        PUTCH(out, '(');
        len += 1+dump_arg(out, start+1, 2, true);
    }
    else if (MATCH("Cl"))
    {
        PUTCH(out, '(');
        len += 1+dump_arg(out, start+1, 4, true);
    }
    else if (MATCH("Vd") || MATCH("VI"))
    {
        PUTCH(out, '(');
        PUTCH(out, *start); len++;
        PUTCH(out, ')');
    }
    else if (MATCH("Vp"))
    {
        /* NOTE: almost certainly needs work */
        PUTCH(out, '(');
        len += 1+dump_arg(out, start+1, 3, true);
    }
    else if (MATCH("Vl") || MATCH("Vi"))
    {
        PUTCH(out, '(');
        len += 1+dump_arg(out, start+1, is_mono_display?6:8, true);
    }
    else if (MATCH("V"))
    {
        PUTCH(out, '(');
        len += 1+dump_arg(out, start+1, is_mono_display?5:7, true);
    }
    else if (MATCH("X"))
    {
        if (*start+1 == 'd')
        {
            fprintf(out, "(d)");
            len ++;
        }
        else
        {
            PUTCH(out, '(');
            len += 1+dump_arg(out, start+1, 1, true);
        }
    }
    else if (MATCH("St") || MATCH("Sx"))
    {
        PUTCH(out, '(');
        len += 1+dump_arg(out, start+1, 1, true);
    }
    
    else if (MATCH("T"))
    {
        PUTCH(out, '(');
        len += 1+dump_arg(out, start+1, 5, true);
    }
    return len;
}
        
void parse_text(const char* in, FILE* out)
{
    const char* end = in+strlen(in);
    int level = 0;
    int len;
top:
    while (in < end && *in)
    {
        if (*in == '%')
        {
            PUTCH(out, *in++);
            switch(*in)
            {

                case '%':
                case '<':
                case '|':
                case '>':
                case ';':
                case '#':
                    PUTCH(out, *in++);
                    goto top;
                    break;
                case '?':
                    PUTCH(out, *in++);
                    break;
            }
            len = parse_tag(out, in, level>0);
            if (len < 0)
            {
                PUTCH(out, *in++);
            }
            else
            {
                in += len;
            }
        }
        else if (*in == '<')
        {
            level++;
            PUTCH(out, *in++);
        }
        else if (*in == '>')
        {
            level--;
            PUTCH(out, *in++);
        }
        else if (*in == '#')
        {
            while (*in && *in != '\n')
            {
                PUTCH(out, *in++);
            }
        }
        else 
        {
            PUTCH(out, *in++);
        }            
    }
}

int main(int argc, char* argv[])
{
    char buffer[10*1024], temp[512];
    FILE *in, *out = stdout;
    int filearg = 1, i=0;
    if( (argc < 2) ||
        strcmp(argv[1],"-h") == 0 ||
        strcmp(argv[1],"--help") == 0 )
    {
        printf("Usage: %s [OPTIONS] infile [outfile]\n", argv[0]);
        printf("\nOPTIONS:\n");
        printf("\t-m\tSkin is for a mono display (different viewport tags)\n");
        return 0;
    }
    
    while ((argc > filearg) && argv[filearg][0] == '-')
    {
        i=1;
        while (argv[filearg][i])
        {
            switch(argv[filearg][i])
            {
                case 'm': /* skin is for a mono display */
                    is_mono_display = true;
                    break;
            }
            i++;
        }
        filearg++;
    }
    if (argc == filearg)
    {
        printf("Missing input filename\n");
        return 1;
    }
    
    in = fopen(argv[filearg], "r");
    if (!in)
        return 1;
    while (fgets(temp, 512, in))
        strcat(buffer, temp);
    fclose(in);
    filearg++;
    
    if (argc > filearg)
    {
        out = fopen(argv[filearg], "w");
        if (!out)
        {
            printf("Couldn't open %s\n", argv[filearg]);
            return 1;
        }
    }        
    
    parse_text(buffer, out);
    if (out != stdout)
        fclose(out);
    return 0;
}
