/*
 * mididings
 *
 * Copyright (C) 2008  Dominic Sacré  <dominic.sacre@gmx.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "backend_jack.hh"
#include "midi_event.hh"
#include "config.hh"

#include <jack/jack.h>
#include <jack/midiport.h>

#include "util/debug.hh"


BackendJack::BackendJack(std::string const & client_name,
                         std::vector<std::string> const & in_portnames,
                         std::vector<std::string> const & out_portnames)
  : _in_ports(in_portnames.size())
  , _out_ports(out_portnames.size())
  , _in_rb(Config::MAX_JACK_EVENTS)
  , _out_rb(Config::MAX_JACK_EVENTS)
{
    ASSERT(!client_name.empty());
    ASSERT(!in_portnames.empty());
    ASSERT(!out_portnames.empty());

    if ((_client = jack_client_open(client_name.c_str(), JackNullOption, NULL)) == 0) {
        throw BackendError("can't connect to jack server");
    }

    jack_set_process_callback(_client, &process_, static_cast<void*>(this));

    for (int n = 0; n < (int)in_portnames.size(); n++) {
        _in_ports[n] = jack_port_register(_client, in_portnames[n].c_str(), JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
        if (_in_ports[n] == NULL) {
            throw BackendError("error creating input port");
        }
    }

    for (int n = 0; n < (int)out_portnames.size(); n++) {
        _out_ports[n] = jack_port_register(_client, out_portnames[n].c_str(), JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
        if (_out_ports[n] == NULL) {
            throw BackendError("error creating output port");
        }
    }

    if (jack_activate(_client)) {
        throw BackendError("can't activate client");
    }
}


BackendJack::~BackendJack()
{
    jack_deactivate(_client);
    jack_client_close(_client);
}


int BackendJack::process_(jack_nframes_t nframes, void *arg)
{
    BackendJack *this_ = static_cast<BackendJack*>(arg);
    return this_->process(nframes);
}


int BackendJack::process(jack_nframes_t nframes)
{
    for (int n = 0; n < (int)_in_ports.size(); ++n)
    {
        void *port_buffer = jack_port_get_buffer(_in_ports[n], nframes);
        int num_events = jack_midi_get_event_count(port_buffer);

        for (int c = 0; c < num_events; c++) {
            jack_midi_event_t jack_ev;
            jack_midi_event_get(&jack_ev, port_buffer, c);

            MidiEvent ev = jack_to_midi_event(jack_ev, n);

            _in_rb.write(ev);
        }

        if (num_events) {
            _cond.notify_one();
        }
    }

    for (int n = 0; n < (int)_out_ports.size(); ++n)
    {
        void *port_buffer = jack_port_get_buffer(_out_ports[n], nframes);
        jack_midi_clear_buffer(port_buffer);
    }

    while (_out_rb.read_space())
    {
        MidiEvent ev;
        _out_rb.read(ev);

        unsigned char data[3];
        std::size_t len;
        int port;
        midi_event_to_jack(ev, data, len, port);

        if (len) {
            void *port_buffer = jack_port_get_buffer(_out_ports[port], nframes);
            jack_midi_event_write(port_buffer, 0, data, len);
        }
    }

    return 0;
}


MidiEvent BackendJack::jack_to_midi_event(jack_midi_event_t const & jack_ev, int port)
{
    MidiEvent ev;

    jack_midi_data_t *data = jack_ev.buffer;

    ev.port = port;
    ev.channel = data[0] & 0x0f;

    switch (data[0] & 0xf0) {
      case 0x90:
        ev.type = MIDI_EVENT_NOTEON;
        ev.note.note = data[1];
        ev.note.velocity = data[2];
        break;
      case 0x80:
        ev.type = MIDI_EVENT_NOTEOFF;
        ev.note.note = data[1];
        ev.note.velocity = data[2];
        break;
      case 0xb0:
        ev.type = MIDI_EVENT_CTRL;
        ev.ctrl.param = data[1];
        ev.ctrl.value = data[2];
        break;
      case 0xe0:
        ev.type = MIDI_EVENT_PITCHBEND;
        ev.ctrl.param = 0;
        ev.ctrl.value = (data[2] << 7 | data[1]) - 8192;
        break;
      case 0xc0:
        ev.type = MIDI_EVENT_PROGRAM;
        ev.ctrl.param = 0;
        ev.ctrl.value = data[1];
        break;
      default:
        ev.type = MIDI_EVENT_NONE;
        break;
    }

    return ev;
}


void BackendJack::midi_event_to_jack(MidiEvent const & ev, unsigned char *data, std::size_t & len, int & port)
{
    switch (ev.type) {
      case MIDI_EVENT_NOTEON:
        len = 3;
        data[0] = 0x90;
        data[1] = ev.note.note;
        data[2] = ev.note.velocity;
        break;
      case MIDI_EVENT_NOTEOFF:
        len = 3;
        data[0] = 0x80;
        data[1] = ev.note.note;
        data[2] = ev.note.velocity;
        break;
      case MIDI_EVENT_CTRL:
        len = 3;
        data[0] = 0xb0;
        data[1] = ev.ctrl.param;
        data[2] = ev.ctrl.value;
        break;
      case MIDI_EVENT_PITCHBEND:
        len = 3;
        data[0] = 0xe0;
        data[1] = (ev.ctrl.value + 8192) % 128;
        data[2] = (ev.ctrl.value + 8192) / 128;
        break;
      case MIDI_EVENT_PROGRAM:
        len = 2;
        data[0] = 0xc0;
        data[1] = ev.ctrl.value;
        break;
      default:
        len = 0;
    }

    data[0] |= ev.channel;

    port = ev.port;
}


void BackendJack::input_event(MidiEvent & ev)
{
    while (!_in_rb.read_space()) {
        boost::mutex::scoped_lock lock(_mutex);
        _cond.wait(lock);
    }

    _in_rb.read(ev);
}


void BackendJack::output_event(MidiEvent const & ev)
{
    _out_rb.write(ev);
}


void BackendJack::drop_input()
{
    _in_rb.reset();
}


void BackendJack::flush_output()
{
}
