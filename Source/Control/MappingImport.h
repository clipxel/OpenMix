#pragma once

#include "MidiEngine.h"

namespace om::MappingImport
{

/** Interoperability: controller mappings that other DJ software installed on this computer already
    has (its built-in tables or the user's own files) are read where they are and translated into
    OpenMix bindings. Nothing is copied anywhere else or uploaded. Three formats are understood:
      · CSV tables ("<model>.midi.csv": function, control type, MIDI message, per-deck channel offsets)
      · XML pairs: a device definition (MIDI message → control name) + a mapper (control → action)
      · one-file XML presets: each control's MIDI message, group ([Channel1]…) and function. */

struct Source
{
    juce::String controller;        // model name, as the controller announces itself over MIDI
    juce::File file;                // the CSV table, or the XML mapper / definition
    juce::File definition;          // XML only: the matching device definition (may be empty)
};

struct Result
{
    std::vector<MidiBinding> bindings;
    juce::StringArray untranslated; // controls that have no OpenMix equivalent (or need decks 3–4)
    juce::String controller;
    juce::String basedOn;           // a mapper without its definition: the OpenMix mapping it was laid over
    int changed = 0;                // … and how many controls it changed there
    bool missingDefinition = false; // a mapper whose definition is not on disk, for a model OpenMix does not know
};

/** Mappings found in the usual places of DJ software installed on this computer. Read-only. */
std::vector<Source> findInstalled();

/** The installed mapping for a MIDI device name, if any (used when a controller is plugged in). */
std::optional<Source> findFor (const juce::String& midiDeviceName);

Result convert (const Source&);
Result fromCsv (const juce::String& csvText);
Result fromXml (const juce::String& definitionXml, const juce::String& mapperXml);
Result fromPreset (const juce::String& presetXml);   // one-file XML presets (<…ControllerPreset>)
/** A mapper alone (controllers the other program supports natively keep their definition built in):
    laid over OpenMix's own mapping for that model. */
Result fromMapperOnly (const juce::String& mapperXml, const juce::String& fallbackName = {});

/** Picks the right parser for a file the user chose (.csv, or a .xml definition / mapper). */
Result fromFile (const juce::File&);
/** Several files chosen together (a definition and its mapper, or a mapper alone). */
Result fromFiles (const juce::Array<juce::File>&);

} // namespace om::MappingImport
