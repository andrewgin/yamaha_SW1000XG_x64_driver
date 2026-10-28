# Instructions and questions for the current hardware test round, shown by
# Run-HwTest.ps1 before and after the test. Rewritten for each round (ASCII).
@{
    Round     = 5
    Before    = @(
        'Keep the card''s output monitored at a comfortable volume.',
        'Close MIDI-OX and any other MIDI program before you press Enter: they stop Windows restarting the card.',
        'If you have a MIDI keyboard or controller, connect it to the card''s MIDI IN; if you have a sound module or keyboard, connect the card''s MIDI OUT to its MIDI IN. Neither is required.',
        'After you press Enter: about 12 seconds in you should hear three rising notes, then three falling notes. Then the script plays four notes through each SW1000 MIDI output and asks after each, and offers a short MIDI IN recording.'
    )
    Extra     = @('Invoke-MidiPortTest.ps1', 'Invoke-MidiInTest.ps1')
    Questions = @(
        'Did you hear the three rising notes and then the three falling notes? (yes / no / describe)',
        'Now open MIDI-OX: are "SW1000 MIDI OUT" and the three Synthesizer ports in its output list, and "SW1000 MIDI IN" in its input list (Options > MIDI Devices)? Do they work? (describe)',
        'Any other sound from the card, such as clicks, hum or noise? (describe, or press Enter for none)'
    )
}
