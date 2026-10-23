#include "PluginEditor.h"
#include "ui/ReducedMotion.h"
#include "ui/Theme.h"

#include <cmath>

namespace sing
{

using namespace ui;

namespace
{
    constexpr int kPanelWidth = 1040, kPanelHeight = 620;
    constexpr double kFrameInterval = 1.0 / 30.0; // 30 fps is plenty for slow twinkles and playheads
    constexpr float kBypassAlpha = 0.45f;          // controls dim to this while bypassed

    static_assert (VoiceHorizon::maxPlayheads == SingProcessor::kMaxPlayheads);

    double nowSeconds() { return juce::Time::getMillisecondCounterHiRes() * 0.001; }

    float defaultValueOf (juce::AudioProcessorValueTreeState& state, const char* id)
    {
        auto* p = state.getParameter (id);
        return p->convertFrom0to1 (p->getDefaultValue());
    }

    // Whole numbers drop their decimals: "2 st", "+2.5 st".
    juce::String trimmed (double v, int decimals)
    {
        const double rounded = std::round (v);
        if (std::abs (v - rounded) < 0.5 * std::pow (10.0, -decimals))
            return juce::String (static_cast<int> (rounded));
        return juce::String (v, decimals);
    }

    juce::String signedValue (double v, int decimals, const juce::String& unit)
    {
        const auto text = trimmed (std::abs (v), decimals);
        if (text == "0")
            return "0 " + unit;
        return (v < 0.0 ? "-" : "+") + text + " " + unit;
    }

    // "350 ms", "2.4 s", "12 s"
    juce::String formatTime (double seconds)
    {
        if (seconds < 0.995)
            return juce::String (juce::roundToInt (seconds * 1000.0)) + " ms";
        if (seconds < 9.95)
            return juce::String (seconds, 1) + " s";
        return juce::String (juce::roundToInt (seconds)) + " s";
    }

    juce::String formatPercent (double v) { return juce::String (juce::roundToInt (v)) + "%"; }

    // "1.44 s" for a point in the voice, or a share of it when there's no voice yet.
    juce::String formatPosition (double fraction, double voiceSeconds)
    {
        if (voiceSeconds <= 0.0)
            return formatPercent (fraction * 100.0);
        return juce::String (fraction * voiceSeconds, 2) + " s";
    }

    juce::String formatGlide (double ms)
    {
        if (ms < 999.5)
            return juce::String (juce::roundToInt (ms)) + " ms";
        return juce::String (ms / 1000.0, 2) + " s";
    }

    juce::String formatOutput (double db)
    {
        if (std::abs (db) < 0.05)
            return "0.0 dB";
        return (db > 0.0 ? "+" : "-") + juce::String (std::abs (db), 1) + " dB";
    }

    juce::String formatVoiceLength (double seconds)
    {
        if (seconds <= 0.0)
            return {};
        return seconds < 9.95 ? juce::String (seconds, 1) + " s" : juce::String (juce::roundToInt (seconds)) + " s";
    }

    // The gold star in the header, and the arrow on the Drop a voice button.
    const juce::Path& logoStar()
    {
        static const juce::Path p = juce::Drawable::parseSVGPath ("M12 1 L13.4 10.6 L23 12 L13.4 13.4 L12 23 L10.6 13.4 L1 12 L10.6 10.6 Z");
        return p;
    }

    const juce::Path& dropArrow()
    {
        static const juce::Path p = juce::Drawable::parseSVGPath ("M7 1 V10 M3 6 L7 10 L11 6 M2 13 H12");
        return p;
    }

    // Draws `text` in one line inside `area`, shrinking the font (down to `minPx`)
    // and then cutting it short with an ellipsis if it still doesn't fit.
    void drawFitted (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area,
                     const std::function<juce::Font (float)>& font, float px, float minPx, juce::Colour colour)
    {
        auto f = font (px);
        const float w = trackedTextWidth (text, f);
        if (w > area.getWidth())
            f = font (std::max (minPx, px * area.getWidth() / w));
        auto shown = text;
        while (shown.length() > 1 && trackedTextWidth (shown, f) > area.getWidth())
            shown = shown.dropLastCharacters (2).trimEnd() + juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xa6"));
        drawTrackedText (g, shown, area, f, juce::Justification::centredLeft, colour);
    }
}

//==============================================================================
// The preset number and name in the middle of the header. Click it for the full list.
class BlueHourPanel::PresetName final : public juce::Button
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
        const float numW = num.isEmpty() ? 0.0f : juce::GlyphArrangement::getStringWidth (font, num) + 10.0f;
        const float total = numW + trackedTextWidth (shown, font);
        const float x = b.getCentreX() - std::min (total, b.getWidth()) * 0.5f;

        if (num.isNotEmpty())
            drawTrackedText (g, num, b.withX (x).withWidth (numW), font, juce::Justification::centredLeft, Colours::muted);
        drawTrackedText (g, shown, b.withX (x + numW).withRight (b.getRight()), font, juce::Justification::centredLeft,
                         highlighted ? Colours::ivory : Colours::text);
    }

private:
    juce::String num, text;
    bool modified = false;
};

//==============================================================================
// A round, see-through button over the evening star. The sky draws the star itself;
// this only adds a faint ring on hover and catches clicks.
class BlueHourPanel::VenusButton final : public juce::Button
{
public:
    VenusButton() : juce::Button ("Bypass")
    {
        setTitle ("Bypass");
        setClickingTogglesState (true);
        setTooltip ("Click the evening star to bypass");
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    bool hitTest (int x, int y) override
    {
        const auto c = getLocalBounds().toFloat().getCentre();
        return c.getDistanceFrom ({ static_cast<float> (x), static_cast<float> (y) }) <= static_cast<float> (getWidth()) * 0.5f;
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool) override
    {
        if (! highlighted && ! hasKeyboardFocus (false))
            return;
        g.setColour (Colours::ivory.withAlpha (0.22f));
        g.drawEllipse (getLocalBounds().toFloat().reduced (1.5f), 1.0f);
    }
};

//==============================================================================
// The dashed "DROP A VOICE" pill. Click it to choose a file; it lights up while a file
// is dragged over the panel.
class BlueHourPanel::DropButton final : public juce::Button
{
public:
    DropButton() : juce::Button ("Drop a voice")
    {
        setTitle ("Choose a voice");
        setTooltip ("Choose an audio file, or drag one anywhere onto Sing");
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    static juce::Font font() { return Fonts::sans (12.0f, false, 0.18f); }
    static float preferredWidth() { return 16.0f + 14.0f + 8.0f + trackedTextWidth ("DROP A VOICE", font()) + 16.0f + 2.0f; }

    void setLit (bool shouldLight)
    {
        if (lit != shouldLight)
        {
            lit = shouldLight;
            repaint();
        }
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        const auto b = getLocalBounds().toFloat().reduced (0.5f);
        const float radius = b.getHeight() * 0.5f;

        g.setColour (lit ? Colours::gold.withAlpha (0.22f) : Colours::dusk.withAlpha (down ? 0.7f : (highlighted ? 0.6f : 0.45f)));
        g.fillRoundedRectangle (b, radius);

        // border: 1px dashed rgba(243,207,138,0.55)
        juce::Path outline, dashed;
        outline.addRoundedRectangle (b, radius);
        const float dashes[] = { 3.0f, 3.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);
        g.setColour (Colours::gold.withAlpha (lit || highlighted ? 0.9f : 0.55f));
        g.fillPath (dashed);

        const float x = b.getX() + 16.0f;
        g.setColour (Colours::gold);
        g.strokePath (dropArrow(), juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded),
                      juce::AffineTransform::translation (x, b.getCentreY() - 7.0f));
        drawTrackedText (g, "DROP A VOICE", b.withTrimmedLeft (x + 22.0f - b.getX()), font(), juce::Justification::centredLeft, Colours::text);
    }

private:
    bool lit = false;
};

//==============================================================================
// Drawn over everything, without catching the mouse: the glow round the panel while a
// file is dragged over it, and "PREPARING VOICE" over the waveform while it loads.
class BlueHourPanel::Veil final : public juce::Component
{
public:
    Veil() { setInterceptsMouseClicks (false, false); }

    void set (bool dragging, bool preparing, float pulse)
    {
        if (dragging != drag || preparing != prep || (preparing && ! juce::approximatelyEqual (pulse, glow)))
        {
            drag = dragging;
            prep = preparing;
            glow = pulse;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto wave = VoiceHorizon::waveArea();
        if (drag)
        {
            const auto frame = getLocalBounds().toFloat().reduced (8.0f);
            drawGlow (g, frame, 12.0f, 14.0f, Colours::gold.withAlpha (0.35f));
            g.setColour (Colours::gold.withAlpha (0.85f));
            g.drawRoundedRectangle (frame, 12.0f, 1.5f);

            const auto label = juce::Rectangle<float> (300.0f, 30.0f).withCentre (wave.getCentre());
            g.setColour (Colours::dusk.withAlpha (0.82f));
            g.fillRoundedRectangle (label, 15.0f);
            drawTrackedText (g, "LET GO TO SING THIS VOICE", label, Fonts::sans (12.0f, false, 0.24f),
                             juce::Justification::centred, Colours::gold);
        }
        else if (prep)
        {
            const auto label = juce::Rectangle<float> (240.0f, 30.0f).withCentre (wave.getCentre());
            g.setColour (Colours::dusk.withAlpha (0.7f));
            g.fillRoundedRectangle (label, 15.0f);
            drawTrackedText (g, juce::CharPointer_UTF8 ("PREPARING VOICE\xe2\x80\xa6"), label, Fonts::sans (12.0f, false, 0.24f),
                             juce::Justification::centred, Colours::text.withAlpha (0.6f + 0.4f * glow));
        }
    }

private:
    bool drag = false, prep = false;
    float glow = 1.0f;
};

//==============================================================================
BlueHourPanel::BlueHourPanel (SingProcessor& p)
    : processor (p), presets (p.getPresets()), presetName (std::make_unique<PresetName>()),
      dropButton (std::make_unique<DropButton>()), horizon (p.getState()),
      veil (std::make_unique<Veil>()), venusButton (std::make_unique<VenusButton>())
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
    addAndMakeVisible (*dropButton);
    dropButton->onClick = [this] { chooseVoiceFile(); };
    addAndMakeVisible (horizon);

    // Loop direction: three pills, one lit.
    const PillToggle::Style small { 11.0f, 0.18f, 14.0f, 0.55f, 0.3f };
    const juce::String modeNames[] = { "FORWARD", "BACKWARD", "BACK & FORTH" };
    for (size_t i = 0; i < loopModes.size(); ++i)
    {
        auto pill = std::make_unique<PillToggle> (modeNames[i], small);
        pill->setClickingTogglesState (false);
        pill->setRadioGroupId (1);
        pill->onClick = [this, i] { loopModeAttachment->setValueAsCompleteGesture (static_cast<float> (i)); };
        addAndMakeVisible (*pill);
        loopModes[i] = std::move (pill);
    }
    loopModeAttachment = std::make_unique<juce::ParameterAttachment> (
        *state.getParameter (ParamID::loopMode), [this] (float value) { setLoopMode (juce::roundToInt (value)); });

    // Start, Length and Fade, dragged up and down (or set on the waveform).
    for (auto* d : { &loopStart, &loopLength, &loopFade })
        addAndMakeVisible (d);
    sliderAttachments.push_back (std::make_unique<SliderAttachment> (state, ParamID::loopStart, loopStart));
    sliderAttachments.push_back (std::make_unique<SliderAttachment> (state, ParamID::loopLength, loopLength));
    sliderAttachments.push_back (std::make_unique<SliderAttachment> (state, ParamID::loopFade, loopFade));
    loopStart.formatValue = [this] (double v) { return formatPosition (v, voice.lengthSeconds); };
    loopLength.formatValue = [this] (double v) { return formatPosition (v, voice.lengthSeconds); };
    loopFade.formatValue = [] (double v) { return formatPercent (v); };
    loopStart.setTooltip ("Where the loop starts");
    loopLength.setTooltip ("How long the loop is");
    loopFade.setTooltip ("How much the loop's ends crossfade. Long fades blur it into a steady tone");
    for (auto [d, id] : { std::pair { &loopStart, ParamID::loopStart }, { &loopLength, ParamID::loopLength }, { &loopFade, ParamID::loopFade } })
        d->setDoubleClickReturnValue (true, defaultValueOf (state, id));

    // Knobs
    const char* ids[numKnobs] = { ParamID::attack, ParamID::release, ParamID::stretch, ParamID::spread,
                                  ParamID::formant, ParamID::air, ParamID::space };
    const char* labels[numKnobs] = { "Attack", "Release", "Stretch", "Spread", "Formant", "Air", "Space" };
    const char* tips[numKnobs] = { "How slowly each note fades in",
                                   "How slowly each note fades out",
                                   "Slows the loop down without changing its pitch, up to a held breath",
                                   "Detuned copies across the stereo field: one singer becomes a choir",
                                   "Moves the vowel's character down (larger, darker) or up (smaller, glassier)",
                                   "A gentle lift on top, with a breath that follows the voice",
                                   "A long reverb that blooms the pad into a wash" };
    for (int i = 0; i < numKnobs; ++i)
    {
        auto& knob = knobs[static_cast<size_t> (i)];
        addAndMakeVisible (knob);
        sliderAttachments.push_back (std::make_unique<SliderAttachment> (state, ids[i], knob));
        knob.setDoubleClickReturnValue (true, defaultValueOf (state, ids[i]));
        knob.setLabel (labels[i]);
        knob.setTooltip (tips[i]);
    }
    knobs[attack].formatValue = knobs[release].formatValue = [] (double v) { return formatTime (v); };
    knobs[stretch].formatValue = knobs[spread].formatValue = knobs[space].formatValue = [] (double v) { return formatPercent (v); };
    knobs[formant].formatValue = [] (double v) { return signedValue (v, 1, "st"); };
    knobs[air].formatValue = [] (double v) { return signedValue (v, 1, "dB"); };

    // Bottom row
    for (auto* c : std::initializer_list<juce::Component*> { &hold, &octDown, &octUp, &tape, &voices, &glide, &output })
        addAndMakeVisible (c);
    buttonAttachments.push_back (std::make_unique<ButtonAttachment> (state, ParamID::hold, hold));
    buttonAttachments.push_back (std::make_unique<ButtonAttachment> (state, ParamID::octDown, octDown));
    buttonAttachments.push_back (std::make_unique<ButtonAttachment> (state, ParamID::octUp, octUp));
    buttonAttachments.push_back (std::make_unique<ButtonAttachment> (state, ParamID::tape, tape));
    hold.setTooltip ("Freeze the sound so the pad holds as long as the key does");
    octDown.setTooltip ("Add a quiet layer an octave below");
    octUp.setTooltip ("Add a quiet layer an octave above");
    tape.setTooltip ("Soft saturation and a little slow wow, for warmth");

    sliderAttachments.push_back (std::make_unique<SliderAttachment> (state, ParamID::voices, voices));
    sliderAttachments.push_back (std::make_unique<SliderAttachment> (state, ParamID::glide, glide));
    sliderAttachments.push_back (std::make_unique<SliderAttachment> (state, ParamID::output, output));
    voices.formatValue = [] (double v) { return juce::String (juce::roundToInt (v)); };
    glide.formatValue = [] (double v) { return formatGlide (v); };
    output.formatValue = [] (double v) { return formatOutput (v); };
    voices.setTooltip ("How many notes can sound at once");
    glide.setTooltip ("How long each note slides to its pitch");
    output.setTooltip ("Output level");
    for (auto [d, id] : { std::pair { &voices, ParamID::voices }, { &glide, ParamID::glide }, { &output, ParamID::output } })
        d->setDoubleClickReturnValue (true, defaultValueOf (state, id));

    addAndMakeVisible (*venusButton);
    venusButton->onClick = [this] { bypassAttachment->setValueAsCompleteGesture (venusButton->getToggleState() ? 1.0f : 0.0f); };
    bypassAttachment = std::make_unique<juce::ParameterAttachment> (
        *state.getParameter (ParamID::bypass), [this] (float value) { setBypassed (value > 0.5f); });

    addAndMakeVisible (*veil);
    addChildComponent (saveOverlay);
    saveOverlay.onSave = [this] (const juce::String& name)
    {
        if (! presets.saveUserPreset (name))
        {
            presetNote = "Couldn't save the preset";
            presetNoteUntil = nowSeconds() + 3.0;
        }
        updatePresetDisplay();
    };

    loopModeAttachment->sendInitialUpdate();
    bypassAttachment->sendInitialUpdate();
    refreshVoice();
    updatePresetDisplay();
    setSize (kPanelWidth, kPanelHeight);
}

BlueHourPanel::~BlueHourPanel() = default;

//==============================================================================
void BlueHourPanel::setLoopMode (int mode)
{
    for (size_t i = 0; i < loopModes.size(); ++i)
        loopModes[i]->setToggleState (static_cast<int> (i) == mode, juce::dontSendNotification);
}

void BlueHourPanel::setBypassed (bool shouldBypass)
{
    bypassed = shouldBypass;
    venusButton->setToggleState (bypassed, juce::dontSendNotification);
    venusButton->setTitle (bypassed ? "Turn Sing on" : "Bypass Sing");
    venusButton->setTooltip (bypassed ? "Click the evening star to turn Sing back on" : "Click the evening star to bypass");
    sky.setBypassed (bypassed);

    // Dim the controls while bypassed; they still work.
    const float alpha = bypassed ? kBypassAlpha : 1.0f;
    horizon.setAlpha (alpha);
    for (auto& m : loopModes)
        m->setAlpha (alpha);
    for (auto& k : knobs)
        k.setAlpha (alpha);
    for (auto* c : std::initializer_list<juce::Component*> { &loopStart, &loopLength, &loopFade, &hold, &octDown, &octUp,
                                                              &tape, &voices, &glide, &output })
        c->setAlpha (alpha);
    repaint();
}

void BlueHourPanel::refreshVoice()
{
    lastVoiceChange = processor.getVoiceChangeCount();
    voice = processor.getVoiceInfo();
    horizon.setWaveform (processor.getWaveform());
    loopStart.repaint();
    loopLength.repaint();
    repaint();
}

//==============================================================================
bool BlueHourPanel::isAudioFile (const juce::String& path)
{
    return juce::File (path).hasFileExtension ("wav;wave;aif;aiff;aifc;flac");
}

bool BlueHourPanel::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (isAudioFile (f))
            return true;
    return false;
}

void BlueHourPanel::fileDragEnter (const juce::StringArray&, int, int)
{
    fileDragging = true;
    dropButton->setLit (true);
    veil->set (true, loading, 1.0f);
}

void BlueHourPanel::fileDragExit (const juce::StringArray&)
{
    fileDragging = false;
    dropButton->setLit (false);
    veil->set (false, loading, 1.0f);
}

void BlueHourPanel::filesDropped (const juce::StringArray& files, int, int)
{
    fileDragging = false;
    dropButton->setLit (false);
    veil->set (false, loading, 1.0f);
    for (const auto& f : files)
        if (isAudioFile (f))
        {
            loadVoice (juce::File (f));
            return;
        }
}

void BlueHourPanel::chooseVoiceFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose a voice for Sing", juce::File(), "*.wav;*.wave;*.aif;*.aiff;*.aifc;*.flac");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer<BlueHourPanel> (this)] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr)
                                  return;
                              const auto file = fc.getResult();
                              if (file != juce::File())
                                  safe->loadVoice (file);
                          });
}

void BlueHourPanel::loadVoice (const juce::File& file)
{
    juce::String error;
    if (! processor.loadVoiceFile (file, error))
        showNote (error.isNotEmpty() ? error : "Sing couldn't use " + file.getFileName());
    else
        voiceNote.clear();
    repaint();
}

void BlueHourPanel::showNote (const juce::String& text, double seconds)
{
    voiceNote = text;
    voiceNoteUntil = nowSeconds() + seconds;
    repaint();
}

//==============================================================================
void BlueHourPanel::updatePresetDisplay()
{
    const bool edited = presets.isModified();
    const int index = presets.getCurrentIndex();

    juce::String number, name;
    if (presetNote.isNotEmpty() && nowSeconds() < presetNoteUntil)
        name = presetNote;
    else
    {
        presetNote.clear();
        number = index >= 0 ? juce::String (index + 1).paddedLeft ('0', 3) : juce::String();
        name = presets.getCurrentName();
    }

    const auto key = number + "|" + name + "|" + (edited ? "*" : "");
    if (key != lastPresetText || presets.isOnB() != lastAB || presets.getChangeCount() != lastPresetChange)
    {
        lastPresetText = key;
        lastAB = presets.isOnB();
        lastPresetChange = presets.getChangeCount();
        presetName->set (number, name, edited && presetNote.isEmpty());
        abButton.setLitSide (presets.isOnB() ? 1 : 0);
    }
}

void BlueHourPanel::showPresetMenu()
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
                        [safe = juce::Component::SafePointer<BlueHourPanel> (this)] (int result)
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
                        });
}

//==============================================================================
void BlueHourPanel::frame (double seconds, bool animate)
{
    const bool motionChanged = animate != motion;
    motion = animate;
    sky.setMotion (animate);
    sky.advance (seconds);

    // A new voice (dropped in, or from a preset or session).
    bool changed = motionChanged;
    if (processor.getVoiceChangeCount() != lastVoiceChange)
    {
        refreshVoice();
        changed = true;
    }
    const bool nowLoading = processor.isVoiceLoading();
    if (nowLoading != loading)
    {
        loading = nowLoading;
        horizon.setLoading (loading);
        changed = true;
    }
    const float pulse = animate ? 0.5f + 0.5f * std::cos (static_cast<float> (seconds) * juce::MathConstants<float>::twoPi / 1.6f) : 1.0f;
    veil->set (fileDragging, loading, pulse);

    if (voiceNote.isNotEmpty() && nowSeconds() > voiceNoteUntil)
    {
        voiceNote.clear();
        changed = true;
    }

    horizon.update (processor.getPlayheads(), animate);

    // The value rows are laid out to fit their text, so lay them out again when it changes.
    juce::String rowText;
    for (auto* d : { &loopStart, &loopLength, &loopFade, &voices, &glide, &output })
        rowText << d->getValueText() << "|";
    if (rowText != lastRowText)
    {
        lastRowText = rowText;
        resized();
    }

    if (animate || changed)
        repaint();

    // The preset name changes when knobs move or the host loads a session.
    const auto now = nowSeconds();
    if (now - lastPresetCheck > 0.2)
    {
        lastPresetCheck = now;
        updatePresetDisplay();
    }
}

void BlueHourPanel::resized()
{
    constexpr float left = 40.0f, right = 1000.0f;

    // Header. The preset box and buttons sit where the mockup puts them.
    presetFrame = { 372.0f, 22.0f, 296.0f, 46.0f };
    previous.setBounds (378, 27, 44, 36);
    next.setBounds (618, 27, 44, 36);
    presetName->setBounds (422, 27, 196, 36);
    const float saveW = saveButton.preferredWidth(), abW = abButton.preferredWidth();
    saveButton.setBounds (juce::Rectangle<float> (854.0f - saveW, 24.0f, saveW, 40.0f).toNearestInt());
    abButton.setBounds (juce::Rectangle<float> (854.0f - saveW - 8.0f - abW, 24.0f, abW, 40.0f).toNearestInt());

    dropButton->setBounds (juce::Rectangle<float> (560.0f, 150.0f, DropButton::preferredWidth(), 40.0f).toNearestInt());
    horizon.setBounds (VoiceHorizon::componentBounds());

    // Loop row, centred on y = 364: LOOP, then the direction pills; Start, Length and
    // Fade against the right edge.
    {
        float x = left + juce::GlyphArrangement::getStringWidth (Fonts::sans (12.0f, false, 0.24f), "LOOP") + 6.0f + 10.0f;
        for (auto& pill : loopModes)
        {
            const float w = pill->preferredWidth();
            pill->setBounds (juce::Rectangle<float> (x, 346.0f, w, 36.0f).toNearestInt());
            x += w + 6.0f;
        }
        float rx = right;
        for (auto* d : { &loopFade, &loopLength, &loopStart })
        {
            const float w = d->preferredWidth();
            d->setBounds (juce::Rectangle<float> (rx - w, 346.0f, w, 36.0f).toNearestInt());
            rx -= w + 28.0f;
        }
    }

    // Knobs: 7 across, 8 apart, dials starting at y = 398.
    const float contentW = right - left;
    const float knobW = (contentW - 6.0f * 8.0f) / 7.0f;
    for (int i = 0; i < numKnobs; ++i)
        knobs[static_cast<size_t> (i)].setBounds (
            juce::Rectangle<float> (left + static_cast<float> (i) * (knobW + 8.0f), 398.0f - Knob::topMargin,
                                    knobW, Knob::topMargin + Knob::dialSize + Knob::textHeight).toNearestInt());

    // Bottom row, y = 556 .. 596.
    {
        float x = left;
        for (auto* pill : { &hold, &octDown, &octUp, &tape })
        {
            const float w = pill->preferredWidth();
            pill->setBounds (juce::Rectangle<float> (x, 556.0f, w, 40.0f).toNearestInt());
            x += w + 10.0f;
        }
        float rx = right;
        for (auto* d : { &output, &glide, &voices })
        {
            const float w = d->preferredWidth();
            d->setBounds (juce::Rectangle<float> (rx - w, 557.0f, w, 40.0f).toNearestInt());
            rx -= w + 26.0f;
        }
    }

    venusButton->setBounds (BlueHourSky::venusButtonBounds().toNearestInt());
    veil->setBounds (getLocalBounds());
    saveOverlay.setBounds (getLocalBounds());
}

void BlueHourPanel::paint (juce::Graphics& g)
{
    sky.draw (g);

    // Logo: a small gold star and the name, centred on y = 44.
    g.setColour (Colours::gold);
    g.fillPath (logoStar(), juce::AffineTransform::translation (40.0f, 32.0f));
    drawTrackedText (g, "SING", { 76.0f, 26.0f, 200.0f, 36.3f }, Fonts::serif (30.0f, false, 0.36f),
                     juce::Justification::centredLeft, Colours::text);

    // Preset box frame.
    g.setColour (Colours::dusk.withAlpha (0.55f));
    g.fillRoundedRectangle (presetFrame, 12.0f);
    g.setColour (Colours::gold.withAlpha (0.3f));
    g.drawRoundedRectangle (presetFrame.reduced (0.5f), 12.0f, 1.0f);

    // The evening star's caption.
    if (bypassed)
        drawTrackedText (g, "BYPASSED", { 836.0f, 158.0f, 130.0f, 16.0f }, Fonts::sans (11.0f, false, 0.28f),
                         juce::Justification::centred, Colours::soft);

    // The voice: its length, name and a line about it.
    {
        const float alpha = bypassed ? kBypassAlpha : 1.0f;
        juce::String caption = "VOICE"; // while a voice loads, the waveform says so
        if (const auto len = formatVoiceLength (voice.lengthSeconds); len.isNotEmpty())
            caption << juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 ")) << len;
        drawTrackedText (g, caption, { 40.0f, 96.0f, 500.0f, 17.3f }, Fonts::sans (12.0f, false, 0.3f),
                         juce::Justification::centredLeft, Colours::soft.withMultipliedAlpha (alpha));

        drawFitted (g, voice.name.isNotEmpty() ? voice.name : juce::String ("No voice"), { 40.0f, 117.3f, 505.0f, 48.3f },
                    [] (float px) { return Fonts::serif (px); }, 46.0f, 28.0f, Colours::ivory.withMultipliedAlpha (alpha));

        juce::String line = voice.description;
        auto colour = Colours::body;
        if (voiceNote.isNotEmpty())
        {
            line = voiceNote;
            colour = Colours::rose;
        }
        else if (line.isEmpty() && voice.name.isNotEmpty())
        {
            line = "Your own voice, sung at "
                 + juce::MidiMessage::getMidiNoteName (juce::roundToInt (voice.rootNote), true, true, 3);
            const int cents = juce::roundToInt ((voice.rootNote - std::round (voice.rootNote)) * 100.0f);
            if (cents != 0)
                line << (cents > 0 ? " +" : " ") << cents << " cents";
        }
        else if (line.isEmpty())
            line = "Drop a WAV or AIFF anywhere here to give Sing a voice";
        drawFitted (g, line, { 40.0f, 169.6f, 505.0f, 20.2f }, [] (float px) { return Fonts::sans (px); }, 14.0f, 12.0f,
                    colour.withMultipliedAlpha (alpha));
    }

    // LOOP, in front of the direction pills.
    drawTrackedText (g, "LOOP", { 40.0f, 346.0f, 80.0f, 36.0f }, Fonts::sans (12.0f, false, 0.24f),
                     juce::Justification::centredLeft, Colours::soft.withMultipliedAlpha (bypassed ? kBypassAlpha : 1.0f));
}

//==============================================================================
SingEditor::SingEditor (SingProcessor& p)
    : AudioProcessorEditor (p), singProcessor (p), panel (p),
      vblank (this, [this] (double t) { onFrame (t); })
{
    // Read the saved size first: setting the limits below calls resized(), which stores a new one.
    const float scale = juce::jlimit (0.7f, 1.5f, singProcessor.getUiScale());

    setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (panel);

    setResizable (true, true);
    setResizeLimits (kPanelWidth * 7 / 10, kPanelHeight * 7 / 10, kPanelWidth * 3 / 2, kPanelHeight * 3 / 2);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio (static_cast<double> (kPanelWidth) / kPanelHeight);

    motion = ! prefersReducedMotion();
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
    singProcessor.setUiScale (scale);
}

void SingEditor::onFrame (double timestamp)
{
    if (lastFrame < 0.0)
        lastFrame = timestamp;
    const double dt = timestamp - lastFrame;
    if (dt < kFrameInterval - 0.004)
        return;
    lastFrame = timestamp;

    // Check the accessibility setting about once a second; it's cheap but not free.
    if (timestamp - lastMotionCheck > 1.0)
    {
        lastMotionCheck = timestamp;
        motion = ! prefersReducedMotion();
    }

    // Pause the clock rather than jump ahead after the window was hidden.
    if (motion)
        animationTime += std::min (dt, 0.1);
    panel.frame (animationTime, motion);
}

} // namespace sing
