#include "PluginEditor.h"
#include "UI/ReducedMotion.h"

#include <algorithm>

namespace sing
{

using namespace ui;

namespace
{
    constexpr int kPanelWidth = (int) ui::panelWidth, kPanelHeight = (int) ui::panelHeight;
    constexpr double kFrameInterval = 1.0 / 30.0;
    const juce::String kDot (juce::CharPointer_UTF8 ("  \xc2\xb7  "));

    float defaultValueOf (juce::AudioProcessorValueTreeState& state, const char* id)
    {
        auto* p = state.getParameter (id);
        return p->convertFrom0to1 (p->getDefaultValue());
    }

    double nowSeconds() { return juce::Time::getMillisecondCounterHiRes() * 0.001; }

    bool isAudioFile (const juce::String& path)
    {
        return juce::File (path).hasFileExtension ("wav;wave;aif;aiff;aifc;flac");
    }
}

//==============================================================================
// The preset number and name in the middle of the header. Click it for the full list.
class SingPanel::PresetName final : public juce::Button
{
public:
    PresetName() : juce::Button ("Preset") { setTitle ("Preset list"); }

    void set (const juce::String& number, const juce::String& name, bool edited)
    {
        num = number;
        text = name;
        modified = edited;
        setDescription (name);
        repaint();
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool) override
    {
        const auto b = getLocalBounds().toFloat();
        const auto font = Fonts::sans (15.0f);
        const auto shown = text + (modified ? "*" : "");
        const auto numText = num.isEmpty() ? juce::String() : num;
        const float numW = numText.isEmpty() ? 0.0f : juce::GlyphArrangement::getStringWidth (font, numText) + 10.0f;
        const float total = numW + trackedTextWidth (shown, font);
        const float x = b.getCentreX() - total * 0.5f;

        drawTrackedText (g, numText, b.withX (x).withWidth (numW + 2.0f), font, juce::Justification::centredLeft, Colours::muted);
        drawTrackedText (g, shown, b.withX (x + numW).withRight (b.getRight()), font, juce::Justification::centredLeft,
                         highlighted ? Colours::ivory : Colours::text);
    }

private:
    juce::String num, text;
    bool modified = false;
};

//==============================================================================
// A round, see-through button over the evening star. The sky draws the star; this adds a
// faint ring on hover and catches clicks.
class SingPanel::StarButton final : public juce::Button
{
public:
    StarButton() : juce::Button ("Bypass")
    {
        setTitle ("Bypass Sing");
        setClickingTogglesState (true);
        setTooltip ("Click the evening star to bypass");
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    bool hitTest (int x, int y) override
    {
        const auto c = getLocalBounds().toFloat().getCentre();
        return c.getDistanceFrom ({ static_cast<float> (x), static_cast<float> (y) }) <= getWidth() * 0.5f;
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool) override
    {
        if (! highlighted && ! hasKeyboardFocus (false))
            return;
        g.setColour (Colours::gold.withAlpha (0.45f));
        g.drawEllipse (getLocalBounds().toFloat().reduced (1.5f), 1.0f);
    }
};

//==============================================================================
// The voice's name, root and length, one small line above the waveform. Click for the root
// note and silence trim.
class SingPanel::VoiceInfo final : public juce::Button
{
public:
    VoiceInfo() : juce::Button ("Voice")
    {
        setTitle ("Voice settings");
        setTooltip ("Root note and silence trim");
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    void set (const juce::String& lengthText, const juce::String& voiceName, const juce::String& line)
    {
        length = lengthText;
        name = voiceName;
        description = line;
        repaint();
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool) override
    {
        const auto b = getLocalBounds().toFloat();
        const auto nameFont = Fonts::sans (12.0f, false, 0.2f);
        const auto shownName = name.toUpperCase();
        drawTrackedText (g, shownName, b, nameFont, juce::Justification::centredLeft, highlighted ? Colours::ivory : Colours::text);
        const float x = trackedTextWidth (shownName, nameFont) + 2.0f;
        drawTrackedText (g, kDot + description + kDot + length, b.withTrimmedLeft (x), Fonts::sans (12.0f, false, 0.04f),
                         juce::Justification::centredLeft, highlighted ? Colours::ivory : Colours::soft);
    }

private:
    juce::String length, name, description;
};

//==============================================================================
SingPanel::SingPanel (SingProcessor& p)
    : processor (p), presets (p.getPresets()), presetName (std::make_unique<PresetName>()),
      voiceInfo (std::make_unique<VoiceInfo>()), waveform (p.getState()), modules (p.getState()),
      starButton (std::make_unique<StarButton>())
{
    setOpaque (true);
    auto& state = processor.getState();

    // Header
    for (auto* c : std::initializer_list<juce::Component*> { &previous, &next, presetName.get(), &abButton, &saveButton })
        addAndMakeVisible (c);
    previous.onClick = [this] { presets.loadPrevious(); updatePresetDisplay(); };
    next.onClick = [this] { presets.loadNext(); updatePresetDisplay(); };
    presetName->onClick = [this] { showPresetMenu(); };
    abButton.setTooltip ("Compare two settings");
    abButton.onClick = [this] { presets.toggleAB(); updatePresetDisplay(); };
    saveButton.onClick = [this]
    {
        const auto name = presets.getCurrentIndex() >= presets.getNumFactoryPresets() ? presets.getCurrentName() : juce::String();
        saveOverlay.show (name);
    };

    // The voice
    addAndMakeVisible (*voiceInfo);
    voiceInfo->onClick = [this] { showVoiceMenu(); };
    addAndMakeVisible (dropButton);
    dropButton.onClick = [this] { chooseVoiceFile(); };
    addAndMakeVisible (waveform);

    // Loop settings
    const auto names = directionNames();
    for (int i = 0; i < (int) directionButtons.size(); ++i)
    {
        auto& b = directionButtons[(size_t) i];
        b = std::make_unique<PillToggle> (names[i].toUpperCase(), 36.0f, 11.0f);
        b->setClickingTogglesState (false);
        b->onClick = [this, i] { directionAttachment->setValueAsCompleteGesture (static_cast<float> (i)); };
        addAndMakeVisible (*b);
    }
    directionAttachment = std::make_unique<juce::ParameterAttachment> (
        *state.getParameter (ParamID::direction),
        [this] (float value)
        {
            const int d = juce::roundToInt (value);
            for (int i = 0; i < (int) directionButtons.size(); ++i)
                directionButtons[(size_t) i]->setToggleState (i == d, juce::dontSendNotification);
            // From start has no loop: only Start applies, so Length and Fade step aside.
            const bool loops = d != (int) LoopDirection::fromStart;
            loopLength.setVisible (loops);
            loopFade.setVisible (loops);
            resized();
        });
    // Keys: how the voice is pitched to each key in Grain.
    const auto modes = pitchModeNames();
    for (int i = 0; i < 2; ++i)
    {
        auto& b = pitchModeButtons[(size_t) i];
        b = std::make_unique<PillToggle> (modes[i].toUpperCase(), 36.0f, 11.0f);
        b->setClickingTogglesState (false);
        b->onClick = [this, i] { pitchModeAttachment->setValueAsCompleteGesture (static_cast<float> (i)); };
        addAndMakeVisible (*b);
    }
    pitchModeButtons[0]->setTooltip ("Formant: in Grain, keys play the voice faster or slower and the formant shifter keeps the vowel; Bloom shifts in its own spectrum");
    pitchModeButtons[1]->setTooltip ("STFT: Grain or Bloom plays at the root and every partial is moved to the key in the spectrum, at the voice's own speed");
    pitchModeAttachment = std::make_unique<juce::ParameterAttachment> (
        *state.getParameter (ParamID::pitchMode), [this] (float) { updatePitchModeButtons(); });

    directionButtons[(size_t) LoopDirection::fromStart]->setTooltip (
        "No loop: each note plays on from Start and holds at the end of the voice. Nothing after Start "
        "blends back in, and Bloom's Window can grow to everything after Start");

    for (auto* d : { &loopStart, &loopLength, &loopFade })
        addAndMakeVisible (d);
    sliderAttachments.push_back (std::make_unique<SliderAttachment> (state, ParamID::loopStart, loopStart));
    sliderAttachments.push_back (std::make_unique<SliderAttachment> (state, ParamID::loopLength, loopLength));
    sliderAttachments.push_back (std::make_unique<SliderAttachment> (state, ParamID::loopFade, loopFade));
    loopStart.formatValue = [this] (double v) { return formatLoopSeconds (v); };
    loopLength.formatValue = [this] (double v) { return formatLoopSeconds (v); };
    loopFade.formatValue = [] (double v) { return formatPercent (static_cast<float> (v)); };
    loopFade.setDoubleClickReturnValue (true, defaultValueOf (state, ParamID::loopFade));
    loopStart.setTooltip ("Where the loop starts. Drag up or down, or drag the handles on the voice");
    loopLength.setTooltip ("How long the loop is");
    loopFade.setTooltip ("Crossfades the loop's ends into each other. Long fades blur the loop into a steady tone");

    addAndMakeVisible (wanderDragger);
    sliderAttachments.push_back (std::make_unique<SliderAttachment> (state, ParamID::wander, wanderDragger));
    wanderDragger.formatValue = [] (double v) { return formatOrbit (static_cast<float> (v)); };
    wanderDragger.setDoubleClickReturnValue (true, 0.0);
    wanderDragger.setTooltip ("Movement for the loop: each note's reading point wanders through the loop, so the vowel slowly changes. "
                              "100% lets it travel the whole loop");
    wanderDragger.onValueChange = [this] { wanderDragger.repaint(); updateMovementStrip(); };

    // Knobs
    const char* ids[numKnobs] = { ParamID::attack, ParamID::release, ParamID::stretch, ParamID::spread, ParamID::formant, ParamID::air, ParamID::space };
    const char* labels[numKnobs] = { "Attack", "Release", "Stretch", "Spread", "Formant", "Air", "Space" };
    for (int i = 0; i < numKnobs; ++i)
    {
        auto& knob = knobs[static_cast<size_t> (i)];
        addAndMakeVisible (knob);
        knob.setLabel (labels[i]);
        sliderAttachments.push_back (std::make_unique<SliderAttachment> (state, ids[i], knob));
        knob.setDoubleClickReturnValue (true, defaultValueOf (state, ids[i]));
    }
    knobs[attack].formatValue = knobs[release].formatValue = [] (double v) { return formatSeconds (static_cast<float> (v)); };
    knobs[stretch].formatValue = [] (double v) { return v >= 97.0 ? juce::String ("Held") : formatPercent (static_cast<float> (v)); };
    knobs[spread].formatValue = [] (double v) { return formatPercent (static_cast<float> (v)); };
    knobs[formant].formatValue = [] (double v) { return formatSemitones (static_cast<float> (v)); };
    knobs[air].formatValue = knobs[space].formatValue = [] (double v) { return formatPercent (static_cast<float> (v)); };
    knobs[attack].setTooltip ("How slowly each note fades in, up to 20 s");
    knobs[release].setTooltip ("How slowly each note fades out after you let go, up to 30 s");
    knobs[stretch].setTooltip ("Slows the voice down without changing its pitch. At the top it holds its breath");
    knobs[spread].setTooltip ("Up to seven detuned copies across the stereo field: one singer becomes a choir");
    knobs[formant].setTooltip ("Moves the vowel's character: down is larger and darker, up is smaller and glassier");
    knobs[air].setTooltip ("A gentle lift at the top, with a soft breath that follows the voice");
    knobs[space].setTooltip ("How loud the reverb is. Set its Decay, Size and cuts in the Space tile below");

    // Orbits: the knobs that can move.
    for (auto [k, id] : { std::pair { stretch, ParamID::orbitStretch }, std::pair { spread, ParamID::orbitSpread },
                          std::pair { formant, ParamID::orbitFormant }, std::pair { air, ParamID::orbitAir },
                          std::pair { space, ParamID::orbitSpace } })
    {
        auto& knob = knobs[(size_t) k];
        knob.setOrbit (*state.getParameter (id));
        knob.setTooltip (knob.getTooltip() + ". Drag the ring around it to make it wander");
    }
    for (auto* id : { ParamID::orbitStretch, ParamID::orbitSpread, ParamID::orbitFormant, ParamID::orbitAir, ParamID::orbitSpace })
        state.addParameterListener (id, this);

    // Bottom row
    addAndMakeVisible (holdButton);
    buttonAttachments.push_back (std::make_unique<ButtonAttachment> (state, ParamID::hold, holdButton));
    holdButton.setTooltip ("Freeze the pad: it holds still for as long as the key is down");
    addAndMakeVisible (keepVowelButton);
    buttonAttachments.push_back (std::make_unique<ButtonAttachment> (state, ParamID::keepVowel, keepVowelButton));
    keepVowelButton.setTooltip ("On: the vowel stays as recorded on every key. Off: plain pitch shifting, so higher keys sound smaller and lower keys larger");

    // The engine: Grain or Bloom.
    const auto engines = engineNames();
    for (int i = 0; i < 2; ++i)
    {
        auto& b = engineButtons[(size_t) i];
        b = std::make_unique<PillToggle> (engines[i].toUpperCase());
        b->setClickingTogglesState (false);
        b->onClick = [this, i] { engineAttachment->setValueAsCompleteGesture (static_cast<float> (i)); };
        addAndMakeVisible (*b);
    }
    engineButtons[0]->setTooltip ("Grain: the voice in tiny overlapping grains, close to how it was sung");
    engineButtons[1]->setTooltip ("Bloom: the voice smeared into a smooth, endless glow, dreamier and slower to change");
    engineAttachment = std::make_unique<juce::ParameterAttachment> (
        *state.getParameter (ParamID::engine),
        [this] (float value)
        {
            bloomEngine = juce::roundToInt (value) == 1;
            for (int i = 0; i < 2; ++i)
                engineButtons[(size_t) i]->setToggleState (i == (bloomEngine ? 1 : 0), juce::dontSendNotification);
            modules.setBloomEngine (bloomEngine);
            repaint();
        });
    for (auto* d : { &voicesDragger, &glideDragger, &outputDragger })
        addAndMakeVisible (d);
    sliderAttachments.push_back (std::make_unique<SliderAttachment> (state, ParamID::voices, voicesDragger));
    sliderAttachments.push_back (std::make_unique<SliderAttachment> (state, ParamID::glide, glideDragger));
    sliderAttachments.push_back (std::make_unique<SliderAttachment> (state, ParamID::output, outputDragger));
    voicesDragger.formatValue = [] (double v) { return juce::String (juce::roundToInt (v)); };
    glideDragger.formatValue = [] (double v) { return formatGlide (static_cast<float> (v)); };
    outputDragger.formatValue = [] (double v) { return formatDecibels (static_cast<float> (v)); };
    for (auto [d, id] : { std::pair { &voicesDragger, ParamID::voices }, std::pair { &glideDragger, ParamID::glide }, std::pair { &outputDragger, ParamID::output } })
        d->setDoubleClickReturnValue (true, defaultValueOf (state, id));
    addAndMakeVisible (modules);

    // Movement strip.
    addAndMakeVisible (shapeButton);
    shapeButton.setTooltip ("How the knobs with a ring move. Drift: wanders at random and never repeats. "
                            "Wave: smooth and even. Breath: a slow swell and a longer fall. Click for the next shape");
    shapeAttachment = std::make_unique<juce::ParameterAttachment> (
        *state.getParameter (ParamID::moveShape),
        [this] (float value)
        {
            const int i = juce::jlimit (0, 2, juce::roundToInt (value));
            shapeButton.setShape (i, moveShapeNames()[i]);
        });
    shapeButton.onClick = [this]
    {
        const int next = (juce::roundToInt (processor.getState().getRawParameterValue (ParamID::moveShape)->load()) + 1) % 3;
        shapeAttachment->setValueAsCompleteGesture (static_cast<float> (next));
    };
    for (auto* d : { &speedDragger, &barsDragger })
        addChildComponent (d);
    sliderAttachments.push_back (std::make_unique<SliderAttachment> (state, ParamID::moveSpeed, speedDragger));
    sliderAttachments.push_back (std::make_unique<SliderAttachment> (state, ParamID::moveBars, barsDragger));
    speedDragger.formatValue = [] (double v) { return formatSeconds (static_cast<float> (v)); };
    barsDragger.formatValue = [] (double v) { return moveBarNames()[juce::jlimit (0, 7, juce::roundToInt (v))]; };
    speedDragger.setDoubleClickReturnValue (true, defaultValueOf (state, ParamID::moveSpeed));
    barsDragger.setDoubleClickReturnValue (true, defaultValueOf (state, ParamID::moveBars));
    speedDragger.setTooltip ("How long one swing takes");
    barsDragger.setTooltip ("How long one swing takes, in bars of the song");
    addAndMakeVisible (syncButton);
    syncButton.setTooltip ("Follow the song: each swing lasts a number of bars, and lines up with them while the song plays");
    syncAttachment = std::make_unique<juce::ParameterAttachment> (
        *state.getParameter (ParamID::moveSync),
        [this] (float value)
        {
            syncButton.setToggleState (value > 0.5f, juce::dontSendNotification);
            updateMovementStrip();
        });
    syncButton.setClickingTogglesState (false);
    syncButton.onClick = [this] { syncAttachment->setValueAsCompleteGesture (syncButton.getToggleState() ? 0.0f : 1.0f); };
    addAndMakeVisible (eachNoteButton);
    buttonAttachments.push_back (std::make_unique<ButtonAttachment> (state, ParamID::moveEach, eachNoteButton));
    eachNoteButton.setTooltip ("On: every note wanders its own way, so a chord shimmers. Off: every note moves together");

    voicesDragger.setTooltip ("How many notes can sound at once");
    glideDragger.setTooltip ("Each new note slides from the last one");

    addAndMakeVisible (*starButton);
    starButton->onClick = [this] { bypassAttachment->setValueAsCompleteGesture (starButton->getToggleState() ? 1.0f : 0.0f); };
    bypassAttachment = std::make_unique<juce::ParameterAttachment> (
        *state.getParameter (ParamID::bypass),
        [this] (float value) { setBypassed (value > 0.5f); });

    addChildComponent (saveOverlay);
    saveOverlay.onSave = [this] (const juce::String& name)
    {
        if (! presets.saveUserPreset (name))
            showNote ("Couldn't save the preset");
        updatePresetDisplay();
    };

    directionAttachment->sendInitialUpdate();
    engineAttachment->sendInitialUpdate();
    pitchModeAttachment->sendInitialUpdate();
    shapeAttachment->sendInitialUpdate();
    syncAttachment->sendInitialUpdate();
    bypassAttachment->sendInitialUpdate();
    updatePresetDisplay();
    updateVoiceDisplay();
    setSize (kPanelWidth, kPanelHeight);
}

SingPanel::~SingPanel()
{
    for (auto* id : { ParamID::orbitStretch, ParamID::orbitSpread, ParamID::orbitFormant, ParamID::orbitAir, ParamID::orbitSpace })
        processor.getState().removeParameterListener (id, this);
}

void SingPanel::parameterChanged (const juce::String&, float)
{
    // May come from the audio thread (automation): the strip is checked on the next frame.
    movementDirty = true;
}

void SingPanel::updateMovementStrip()
{
    const bool sync = syncButton.getToggleState();
    speedDragger.setVisible (! sync);
    barsDragger.setVisible (sync);

    // The strip is quieter while nothing is set to move.
    bool moving = false;
    for (auto* id : { ParamID::orbitStretch, ParamID::orbitSpread, ParamID::orbitFormant, ParamID::orbitAir, ParamID::orbitSpace, ParamID::wander })
        moving = moving || processor.getState().getRawParameterValue (id)->load() > 0.0f;
    const float alpha = (bypassed ? 0.45f : 1.0f) * (moving ? 1.0f : 0.55f);
    for (auto* c : std::initializer_list<juce::Component*> { &shapeButton, &speedDragger, &barsDragger, &syncButton, &eachNoteButton })
        c->setAlpha (alpha);
}

juce::String SingPanel::formatLoopSeconds (double percent) const
{
    const auto v = processor.getVoice();
    const double seconds = v != nullptr ? percent * 0.01 * v->trimSeconds() : 0.0;
    return juce::String (seconds, 2) + " s";
}

void SingPanel::setBypassed (bool shouldBypass)
{
    bypassed = shouldBypass;
    starButton->setToggleState (bypassed, juce::dontSendNotification);
    starButton->setTitle (bypassed ? "Turn Sing on" : "Bypass Sing");
    starButton->setTooltip (bypassed ? "Click the evening star to turn Sing back on" : "Click the evening star to bypass");
    sky.setBypassed (bypassed);

    // Dim the controls while bypassed; they still work.
    const float alpha = bypassed ? 0.45f : 1.0f;
    for (auto* c : std::initializer_list<juce::Component*> { voiceInfo.get(), &waveform, &loopStart, &loopLength, &loopFade, &wanderDragger,
                                                             &holdButton, &keepVowelButton, &voicesDragger, &glideDragger, &outputDragger,
                                                             &modules })
        c->setAlpha (alpha);
    for (auto& b : directionButtons)
        b->setAlpha (alpha);
    for (auto& b : engineButtons)
        b->setAlpha (alpha);
    updatePitchModeButtons();
    for (auto& k : knobs)
        k.setAlpha (alpha);
    updateMovementStrip();
    repaint();
}

void SingPanel::updatePitchModeButtons()
{
    const int mode = juce::roundToInt (processor.getState().getRawParameterValue (ParamID::pitchMode)->load());
    const float alpha = bypassed ? 0.45f : 1.0f;
    for (int i = 0; i < 2; ++i)
    {
        pitchModeButtons[(size_t) i]->setToggleState (i == mode, juce::dontSendNotification);
        pitchModeButtons[(size_t) i]->setAlpha (alpha);
    }
    repaint();
}

void SingPanel::showNote (const juce::String& text)
{
    note = text;
    noteUntil = nowSeconds() + 4.0;
    updatePresetDisplay();
    updateVoiceDisplay();
}

void SingPanel::updatePresetDisplay()
{
    const bool edited = presets.isModified();
    const int index = presets.getCurrentIndex();
    const auto number = index >= 0 ? juce::String (index + 1).paddedLeft ('0', 3) : juce::String();
    const auto name = presets.getCurrentName();

    const auto key = number + "|" + name + "|" + (edited ? "*" : "");
    if (key != lastPresetText || presets.isOnB() != lastAB || presets.getChangeCount() != lastPresetChange)
    {
        lastPresetText = key;
        lastAB = presets.isOnB();
        lastPresetChange = presets.getChangeCount();
        presetName->set (number, name, edited);
        abButton.setLitSide (presets.isOnB() ? 1 : 0);
    }
}

void SingPanel::updateVoiceDisplay()
{
    const auto v = processor.getVoice();
    waveform.setVoice (v);
    if (v == nullptr)
        return;
    shownVoice = v->getId();

    juce::String line;
    if (note.isNotEmpty() && nowSeconds() < noteUntil)
        line = note;
    else
    {
        note.clear();
        if (v->isPitched())
        {
            const float root = v->rootHz();
            const int midi = juce::roundToInt (12.0f * std::log2 (root / 440.0f) + 69.0f);
            line = "Root " + noteName (midi) + (v->getRootOverride() ? " (set by you)" : "") + kDot + v->getDescription();
        }
        else
            line = "Unpitched" + kDot + v->getDescription();
    }

    const double secs = v->trimSeconds();
    voiceInfo->set (juce::String (secs, 1) + " s", v->getName(), line);
    loopStart.repaint();
    loopLength.repaint();
}

void SingPanel::showPresetMenu()
{
    presets.refreshUserPresets();

    juce::PopupMenu menu;
    const int factoryCount = presets.getNumFactoryPresets();
    const int total = presets.getNumPresets();
    const int current = presets.getCurrentIndex();

    menu.addSectionHeader ("FACTORY");
    for (int i = 0; i < factoryCount; ++i)
        menu.addItem (i + 1, juce::String (i + 1).paddedLeft ('0', 3) + "   " + presets.getPresetName (i), true, i == current);

    menu.addSectionHeader ("YOURS");
    if (total == factoryCount)
        menu.addItem (-1, "Save a preset and it shows up here", false);
    for (int i = factoryCount; i < total; ++i)
        menu.addItem (i + 1, juce::String (i + 1).paddedLeft ('0', 3) + "   " + presets.getPresetName (i), true, i == current);

    menu.addSeparator();
    constexpr int kShowFolder = 10000;
    menu.addItem (kShowFolder, "Show presets folder");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (presetName.get()).withMinimumWidth (260),
                        [safe = juce::Component::SafePointer<SingPanel> (this)] (int result)
                        {
                            if (safe == nullptr || result == 0)
                                return;
                            if (result == kShowFolder)
                            {
                                auto folder = PresetManager::getUserPresetFolder();
                                folder.createDirectory();
                                folder.revealToUser();
                                return;
                            }
                            safe->presets.loadPreset (result - 1);
                            safe->updatePresetDisplay();
                            safe->updateVoiceDisplay();
                        });
}

void SingPanel::showVoiceMenu()
{
    const auto v = processor.getVoice();
    if (v == nullptr)
        return;

    constexpr int kAuto = 1, kTrim = 2, kChoose = 3, kNoteBase = 1000;
    juce::PopupMenu menu, notes;

    const auto detected = v->getDetectedHz();
    const auto autoText = detected > 0.0f
        ? "Found automatically: " + noteName (juce::roundToInt (12.0f * std::log2 (detected / 440.0f) + 69.0f))
              + " (" + juce::String (detected, 1) + " Hz)"
        : juce::String ("Found automatically: unpitched (keys map from C4)");

    const auto override = v->getRootOverride();
    for (int octave = 1; octave <= 6; ++octave)
    {
        juce::PopupMenu sub;
        for (int n = 0; n < 12; ++n)
        {
            const int midi = 12 * (octave + 1) + n;
            sub.addItem (kNoteBase + midi, noteName (midi), true, override && *override == midi);
        }
        notes.addSubMenu ("Octave " + juce::String (octave), sub);
    }

    menu.addSectionHeader ("ROOT NOTE");
    menu.addItem (kAuto, autoText, true, ! override.has_value());
    menu.addSubMenu ("Set the root yourself", notes);
    menu.addSeparator();
    menu.addItem (kTrim, "Trim silence from the start and end", true, v->isTrimmed());
    menu.addSeparator();
    menu.addItem (kChoose, "Choose another voice...");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (voiceInfo.get()).withMinimumWidth (300),
                        [safe = juce::Component::SafePointer<SingPanel> (this), v] (int result)
                        {
                            if (safe == nullptr || result == 0)
                                return;
                            if (result == kChoose)
                                safe->chooseVoiceFile();
                            else if (result == kAuto)
                                safe->processor.setVoice (v->withRootOverride (std::nullopt));
                            else if (result == kTrim)
                                safe->processor.setVoice (v->withTrim (! v->isTrimmed()));
                            else if (result >= kNoteBase)
                                safe->processor.setVoice (v->withRootOverride (result - kNoteBase));
                            safe->updateVoiceDisplay();
                        });
}

void SingPanel::chooseVoiceFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose a voice", juce::File(), "*.wav;*.wave;*.aif;*.aiff;*.aifc;*.flac");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer<SingPanel> (this)] (const juce::FileChooser& fc)
                          {
                              if (safe != nullptr && fc.getResult().existsAsFile())
                                  safe->loadVoice (fc.getResult());
                          });
}

void SingPanel::loadVoice (const juce::File& file)
{
    juce::String error;
    if (! processor.loadVoiceFile (file, error))
        showNote (error);
    updateVoiceDisplay();
    updatePresetDisplay();
}

bool SingPanel::isInterestedInFileDrag (const juce::StringArray& files)
{
    return std::any_of (files.begin(), files.end(), isAudioFile);
}

void SingPanel::fileDragEnter (const juce::StringArray&, int, int) { dropButton.setHighlighted (true); }
void SingPanel::fileDragExit (const juce::StringArray&) { dropButton.setHighlighted (false); }

void SingPanel::filesDropped (const juce::StringArray& files, int, int)
{
    dropButton.setHighlighted (false);
    for (const auto& f : files)
        if (isAudioFile (f))
        {
            loadVoice (juce::File (f));
            return;
        }
}

void SingPanel::frame (double seconds, bool animate)
{
    motion = animate;
    // The fireflies follow Space, Spread and Stretch, moving with their orbits.
    {
        const auto orbitsNow = processor.getOrbitPositions();
        auto& state = processor.getState();
        const auto knob = [&state, &orbitsNow] (const char* id, int target, float scale)
        {
            const float live = orbitsNow[(size_t) target];
            return live >= 0.0f ? live : state.getRawParameterValue (id)->load() * scale;
        };
        sky.setKnobs (knob (ParamID::space, moveSpace, 0.01f), knob (ParamID::spread, moveSpread, 0.01f),
                      knob (ParamID::stretch, moveStretch, 0.01f));
    }
    sky.setMotion (animate);
    sky.advance (seconds);
    lastFrameTime = seconds;
    if (animate)
        repaint();

    // The play lights. They are motion too, so they hide when Reduce Motion is on.
    const auto found = processor.getLights();
    static_assert (SingProcessor::maxLights == WaveformView::maxLights);
    std::array<WaveformView::Light, WaveformView::maxLights> lights {};
    for (size_t i = 0; i < lights.size(); ++i)
    {
        const auto& f = found[i];
        lights[i] = { f.position, f.level, f.weight, f.pan, f.pitchRatio, f.zone, f.note, f.copy, f.noteId };
    }
    waveform.setLights (lights, animate, seconds);

    // The lights that circle the knobs with an orbit. Motion too, so they hide under Reduce Motion.
    const auto orbits = processor.getOrbitPositions();
    knobs[stretch].setOrbitLive (animate ? orbits[moveStretch] : -1.0f);
    knobs[spread].setOrbitLive (animate ? orbits[moveSpread] : -1.0f);
    knobs[formant].setOrbitLive (animate ? orbits[moveFormant] : -1.0f);
    knobs[air].setOrbitLive (animate ? orbits[moveAir] : -1.0f);
    knobs[space].setOrbitLive (animate ? orbits[moveSpace] : -1.0f);
    if (movementDirty.exchange (false))
        updateMovementStrip();

    // On the dark night sky the bottom row and the voice's shadow use light colours.
    for (auto* d : { &voicesDragger, &glideDragger, &outputDragger, &speedDragger, &barsDragger })
        d->setOnSunset (false);
    waveform.setDarkSky (true);

    // The fireflies glow brighter while notes sound.
    float level = 0.0f;
    for (const auto& f : found)
        if (f.note >= 0)
            level = std::max (level, f.level);
    sky.setActivity (level);
    modules.frame (seconds, animate, level);

    const auto now = nowSeconds();
    if (now - lastPresetCheck > 0.2)
    {
        lastPresetCheck = now;
        updatePresetDisplay();
        const auto v = processor.getVoice();
        if ((v != nullptr && v->getId() != shownVoice) || (note.isNotEmpty() && now >= noteUntil))
            updateVoiceDisplay();
    }
}

void SingPanel::resized()
{
    constexpr float W = ui::panelWidth, left = 40.0f, right = W - 40.0f, inner = right - left;

    // Header: the preset box in the middle, A / B and Save to its right, the star at the end.
    presetFrame = { (W - 296.0f) * 0.5f, 22.0f, 296.0f, 46.0f };
    const int px = juce::roundToInt (presetFrame.getX());
    previous.setBounds (px + 6, 27, 44, 36);
    next.setBounds (px + 246, 27, 44, 36);
    presetName->setBounds (px + 50, 27, 196, 36);

    auto buttonWidth = [] (const juce::String& t) { return 32.0f + trackedTextWidth (t, Fonts::sans (12.0f, false, 0.18f)) + 2.0f; };
    const float saveW = buttonWidth ("SAVE"), abW = buttonWidth ("A / B");
    const float abX = presetFrame.getRight() + 40.0f;
    abButton.setBounds (juce::Rectangle<float> (abX, 24.0f, abW, 40.0f).toNearestInt());
    saveButton.setBounds (juce::Rectangle<float> (abX + abW + 8.0f, 24.0f, saveW, 40.0f).toNearestInt());

    // The voice: a small caption and the drop button, then the waveform across the panel.
    const float dropW = dropButton.preferredWidth();
    voiceInfo->setBounds (juce::Rectangle<float> (left, 80.0f, inner - dropW - 24.0f, 24.0f).toNearestInt());
    dropButton.setBounds (juce::Rectangle<float> (right - dropW, 77.0f, dropW, 30.0f).toNearestInt());
    waveform.setBounds (juce::Rectangle<float> (left - WaveformView::margin, 114.0f - WaveformView::top,
                                                WaveformView::width, WaveformView::height).toNearestInt());

    // Loop row: LOOP and the direction pills on the left, Start/Length/Fade on the right.
    const float loopY = 240.0f;
    const float loopLabelW = trackedTextWidth ("LOOP", Fonts::sans (12.0f, false, 0.24f)) + 6.0f;
    float x = left + loopLabelW + 10.0f;
    for (auto& b : directionButtons)
    {
        const float w = b->preferredWidth();
        b->setBounds (juce::Rectangle<float> (x, loopY, w, 36.0f).toNearestInt());
        x += w + 6.0f;
    }
    // Keys: the pitch mode, after a gap.
    x += 18.0f;
    keysCaptionX = x;
    x += trackedTextWidth ("KEYS", Fonts::sans (12.0f, false, 0.24f)) + 10.0f;
    for (auto& b : pitchModeButtons)
    {
        const float w = b->preferredWidth();
        b->setBounds (juce::Rectangle<float> (x, loopY, w, 36.0f).toNearestInt());
        x += w + 6.0f;
    }
    float rx = right;
    for (auto [d, widest] : { std::pair { &loopFade, juce::String ("100%") }, std::pair { &loopLength, juce::String ("60.00 s") },
                              std::pair { &loopStart, juce::String ("60.00 s") }, std::pair { &wanderDragger, juce::String ("100%") } })
    {
        if (! d->isVisible() && (d == &loopFade || d == &loopLength))
            continue;
        const float w = d->preferredWidth (widest);
        d->setBounds (juce::Rectangle<float> (rx - w, loopY, w, 36.0f).toNearestInt());
        rx -= w + 16.0f;
    }

    // Knobs: seven across, dials starting at y = 290.
    const float knobW = (inner - 6.0f * 8.0f) / static_cast<float> (numKnobs);
    for (int i = 0; i < numKnobs; ++i)
        knobs[static_cast<size_t> (i)].setBounds (
            juce::Rectangle<float> (left + static_cast<float> (i) * (knobW + 8.0f), 290.0f - Knob::topMargin, knobW, Knob::height).toNearestInt());

    // Every module, in three rows.
    modules.setBounds (juce::Rectangle<float> (left, 430.0f, inner, ModulesPanel::preferredHeight()).toNearestInt());

    // Bottom row.
    const float bottomY = ui::panelHeight - 54.0f;
    holdButton.setBounds (juce::Rectangle<float> (left, bottomY, holdButton.preferredWidth(), 40.0f).toNearestInt());
    float ex = left + holdButton.preferredWidth() + 22.0f;
    for (auto& b : engineButtons)
    {
        const float w = b->preferredWidth();
        b->setBounds (juce::Rectangle<float> (ex, bottomY, w, 40.0f).toNearestInt());
        ex += w + 6.0f;
    }
    keepVowelButton.setBounds (juce::Rectangle<float> (ex + 16.0f, bottomY, keepVowelButton.preferredWidth(), 40.0f).toNearestInt());

    // Movement strip, after Keep vowel: the shape, the speed, Sync and Each note.
    float mx = static_cast<float> (keepVowelButton.getRight()) + 34.0f;
    const float my = bottomY + 4.0f;
    shapeButton.setBounds (juce::Rectangle<float> (mx, my, shapeButton.preferredWidth(), 32.0f).toNearestInt());
    mx += shapeButton.preferredWidth() + 16.0f;
    const float speedW = std::max (speedDragger.preferredWidth ("120 s"), barsDragger.preferredWidth ("32 bars"));
    for (auto* d : { &speedDragger, &barsDragger })
        d->setBounds (juce::Rectangle<float> (mx, bottomY + 6.0f, speedW, 28.0f).toNearestInt());
    mx += speedW + 10.0f;
    syncButton.setBounds (juce::Rectangle<float> (mx, my, syncButton.preferredWidth(), 32.0f).toNearestInt());
    mx += syncButton.preferredWidth() + 6.0f;
    eachNoteButton.setBounds (juce::Rectangle<float> (mx, my, eachNoteButton.preferredWidth(), 32.0f).toNearestInt());
    rx = right;
    for (auto [d, widest] : { std::pair { &outputDragger, juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92" "36.0 dB")) }, std::pair { &glideDragger, juce::String ("2.0 s") },
                              std::pair { &voicesDragger, juce::String ("16") } })
    {
        const float w = d->preferredWidth (widest);
        d->setBounds (juce::Rectangle<float> (rx - w, bottomY + 6.0f, w, 28.0f).toNearestInt());
        rx -= w + 26.0f;
    }

    starButton->setBounds (BlueHourSky::starButtonBounds().toNearestInt());
    saveOverlay.setBounds (getLocalBounds());
}

void SingPanel::paint (juce::Graphics& g)
{
    sky.draw (g);

    // Logo: a small gold star and the name.
    g.setColour (Colours::gold);
    g.fillPath (BlueHourSky::starShape (11.0f), juce::AffineTransform::translation (52.0f, 45.0f));
    drawTrackedText (g, "SING", { 76.0f, 22.0f, 200.0f, 46.0f }, Fonts::serif (30.0f, false, 0.36f),
                     juce::Justification::centredLeft, Colours::text);

    // Preset box frame.
    g.setColour (Colours::night.withAlpha (0.55f));
    g.fillRoundedRectangle (presetFrame, 12.0f);
    g.setColour (Colours::gold.withAlpha (0.3f));
    g.drawRoundedRectangle (presetFrame.reduced (0.5f), 12.0f, 1.0f);

    // LOOP caption.
    g.setOpacity (bypassed ? 0.45f : 1.0f);
    drawTrackedText (g, "LOOP", { 40.0f, 240.0f, 80.0f, 36.0f }, Fonts::sans (12.0f, false, 0.24f),
                     juce::Justification::centredLeft, Colours::soft.withMultipliedAlpha (bypassed ? 0.45f : 1.0f));

    // KEYS caption.
    drawTrackedText (g, "KEYS", { keysCaptionX, 240.0f, 60.0f, 36.0f }, Fonts::sans (12.0f, false, 0.24f),
                     juce::Justification::centredLeft, Colours::soft.withMultipliedAlpha (bypassed ? 0.45f : 1.0f));

    // Hold's glow, like a CSS box-shadow under the lit pill, and the lit engine's.
    if (holdButton.getToggleState())
        drawGlow (g, holdButton.getBounds().toFloat(), 20.0f, 16.0f, Colours::gold.withAlpha (0.5f));
    if (keepVowelButton.getToggleState())
        drawGlow (g, keepVowelButton.getBounds().toFloat(), 20.0f, 12.0f, Colours::gold.withAlpha (0.3f));
    for (auto& b : engineButtons)
        if (b->getToggleState())
            drawGlow (g, b->getBounds().toFloat(), 20.0f, 12.0f, Colours::gold.withAlpha (0.3f));

    if (bypassed)
    {
        const auto star = BlueHourSky::starCentre();
        drawTrackedText (g, "BYPASSED", { star.x - 190.0f, star.y - 8.0f, 150.0f, 16.0f }, Fonts::sans (11.0f, false, 0.28f),
                         juce::Justification::centredRight, Colours::soft);
    }
}

//==============================================================================
SingEditor::SingEditor (SingProcessor& p)
    : AudioProcessorEditor (p), processor (p), panel (p),
      vblank (this, [this] (double t) { onFrame (t); })
{
    // Read the saved size first: setting the limits below calls resized(), which stores a new one.
    const float scale = juce::jlimit (0.6f, 1.4f, processor.getUiScale());

    setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (panel);

    setResizable (true, true);
    setResizeLimits (kPanelWidth * 6 / 10, kPanelHeight * 6 / 10, kPanelWidth * 7 / 5, kPanelHeight * 7 / 5);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio (static_cast<double> (kPanelWidth) / kPanelHeight);

    motion = ! ui::prefersReducedMotion();
    panel.frame (0.0, motion);
    setSize (juce::roundToInt (kPanelWidth * scale), juce::roundToInt (kPanelHeight * scale));
}

SingEditor::~SingEditor()
{
    setLookAndFeel (nullptr);
}

void SingEditor::resized()
{
    const float scale = static_cast<float> (getWidth()) / kPanelWidth;
    panel.setTransform (juce::AffineTransform::scale (scale));
    processor.setUiScale (scale);
}

void SingEditor::onFrame (double timestamp)
{
    if (lastFrame < 0.0)
        lastFrame = timestamp;
    const double dt = timestamp - lastFrame;
    if (dt < kFrameInterval - 0.004)
        return;
    lastFrame = timestamp;

    // Check the accessibility setting about once a second.
    if (timestamp - lastMotionCheck > 1.0)
    {
        lastMotionCheck = timestamp;
        const bool wanted = ! ui::prefersReducedMotion();
        if (wanted != motion)
        {
            motion = wanted;
            panel.repaint();
        }
    }

    // Pause the clock rather than jump ahead after the window was hidden.
    if (motion)
        animationTime += std::min (dt, 0.1);
    panel.frame (animationTime, motion);
}

} // namespace sing
