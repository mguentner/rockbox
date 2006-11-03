#!/bin/sh
#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
# $Id$
#
# Copyright 2006 Jonas Häggqvist, some parts Copyright 2004 Daniel Gudlat
#
# All files in this archive are subject to the GNU General Public License.
# See the file COPYING in the source tree root for full license agreement.
#
# This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
# KIND, either express or implied.

# Include voicecommon.sh from the same dir as this script
# Any settings from voicecommon can be overridden if added below the following
# line.
source `dirname $0`'/voicecommon.sh'

####################
# General settings #
####################

# These settings can be overridden by passing a file with definitions as
# the fourth parameter to this script

# which TTS engine to use. Available: festival, flite, espeak
TTS_ENGINE=festival
# which encoder to use, available: lame, speex, vorbis (only lame will produce
# functional voice clips at this point)
ENCODER=lame
# Where to save temporary files
TEMPDIR=/tmp

###################
# End of settings #
###################

createvoicefile() {
    $VOICEFONT "$LANG_FILE" "$TEMPDIR/" "./$RLANG.voice"
}

deletefiles() {
    # XXX: might be unsafe depending on the value of TEMPDIR
    rm -f "${TEMPDIR}"/LANG_*
    rm -f "${TEMPDIR}"/VOICE_*
}

generateclips() {
    ROCKBOX_DIR="$1"
    RLANG="$2"
    TARGET="$3"
    GENLANG="$ROCKBOX_DIR"/tools/genlang
    ENGLISH="$ROCKBOX_DIR"/apps/lang/english.lang
    LANG_FILE="$ROCKBOX_DIR"/apps/lang/$RLANG.lang

    $GENLANG -e=$ENGLISH -o -t=$TARGET $LANG_FILE |(
    i=0
    while read line; do
        case `expr $i % 3` in
            0)
                # String ID no.
                NUMBER=`echo $line |cut -b 2-`
                ;;
            1)
                # String ID
                ID=`echo $line |cut -b 5-`
                ;;
            2)
                # String
                STRING=`echo $line |cut -b 8-`

                # Now generate the file
                voice "$STRING" "$TEMPDIR/$ID".wav
                encode "$TEMPDIR/$ID".wav "$TEMPDIR/$ID".mp3
                ;;
        esac
        i=`expr $i + 1`
    done
    )
}

if [ -z "$3" ]; then
    echo "Usage: $0 rockboxdirectory language target [settingsfile]";
    exit 32
else
    if [ ! -d "$1" ] || [ ! -f "$1/tools/genlang" ]; then
        echo "Error: $1 is not a Rockbox directory"
        exit 33
    fi
    if [ ! -f "$1/apps/lang/$2.lang" ]; then
        echo "Error: $2 is not a valid language"
        exit 34
    fi
    if [ ! -z "$4" ]; then
        if [ -f "$4" ]; then
            # Read settings from file
            source "$4"
        else
            echo "Error: $4 does not exist"
            exit 36
        fi
    fi
    # XXX: check for valid $TARGET?
fi

VOICEFONT=`dirname $0`/voicefont
if [ ! -x $VOICEFONT ]; then
    echo "Error: $VOICEFONT does not exist or is not executable"
    exit 35
fi

init_tts
init_encoder
generateclips "$1" "$2" "$3"
stop_tts
createvoicefile
#deletefiles
