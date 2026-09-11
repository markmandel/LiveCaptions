/* livecaptions-application.h
 *
 * Copyright 2022 abb128
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <adwaita.h>
#include "audiocap.h"
#include "livecaptions-window.h"
#include "dbus-interface.h"

struct _LiveCaptionsApplication {
    AdwApplication parent_instance;

    GSimpleAction *mic_action;

    GSettings *settings;
    LiveCaptionsWindow *window;
    GtkWindow *welcome;

    asr_thread asr;
    audio_thread audio;

    DBLCapNetSapplesLiveCaptionsExternal *dbus_external;
};

G_BEGIN_DECLS

#define LIVECAPTIONS_TYPE_APPLICATION (livecaptions_application_get_type())

G_DECLARE_FINAL_TYPE (LiveCaptionsApplication, livecaptions_application, LIVECAPTIONS, APPLICATION, AdwApplication)


void livecaptions_application_finish_setup(LiveCaptionsApplication *self, gdouble result);

LiveCaptionsApplication *livecaptions_application_new (gchar *application_id,
                                                       GApplicationFlags  flags);

void livecaptions_application_stream_text(LiveCaptionsApplication *self, const char* text);

void livecaptions_application_rename_speaker(LiveCaptionsApplication *self,
                                             int32_t speaker_id,
                                             const char *name);

// Called once the user has named a speaker, so a window showing the transcript
// can redraw it
typedef void (*LiveCaptionsSpeakerNamed)(gpointer userdata);

// Asks for a name for this speaker and applies it everywhere. Shared so the
// captions and the transcript window offer exactly the same thing.
void livecaptions_application_ask_speaker_name(LiveCaptionsApplication *self,
                                               GtkWindow *parent,
                                               int32_t speaker_id,
                                               LiveCaptionsSpeakerNamed done,
                                               gpointer userdata);

size_t livecaptions_application_get_speaker_spans(LiveCaptionsApplication *self,
                                                  struct line_speaker_span *out,
                                                  size_t max);

size_t livecaptions_application_voice_count(LiveCaptionsApplication *self);
void livecaptions_application_forget_voices(LiveCaptionsApplication *self);

G_END_DECLS
