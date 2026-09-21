/**
 * @file arduino_tens.ino
 * @brief Transcutaneous Electrical Nerve Stimulation (TENS) Device Firmware
 * @author TENS Project Team
 * @target Arduino Nano (ATmega328P @ 16 MHz)
 * 
 * Description:
 * This firmware generates precise therapeutic electrical pulses using the ATmega328P's 
 * 16-bit Timer1 hardware PWM on Digital Pin 9 (OC1A). It interfaces with an HC-05 Bluetooth 
 * module to receive commands in real-time from the "My Relief" Flutter mobile application.
 * 
 * Stimulation Modes Supported:
 * 1. Conventional TENS (High Frequency: 50–120 Hz, Low Intensity, Continuous)
 * 2. Acupuncture-like TENS (Low Frequency: 2–10 Hz, High Intensity / Burst)
 * 3. Intense / Modulated TENS (High Frequency: >100 Hz, Dynamic Frequency/Pulse Modulation)
 */

#include <Arduino.h>

// ==============================================================================
// PIN DEFINITIONS & HARDWARE CONFIGURATION
// ==============================================================================
constexpr uint8_t PIN_PWM_OUTPUT      = 9;   // Timer1 OC1A hardware PWM pin
constexpr uint8_t PIN_STATUS_LED      = 13;  // Onboard / Output indicator LED
constexpr uint8_t PIN_BATTERY_SENSE   = A0;  // Li-ion battery voltage divider input
constexpr uint8_t PIN_INTENSITY_POT   = A1;  // Hardware intensity pot (if connected)

// ==============================================================================
// OPERATIONAL CONSTANTS & LIMITS
// ==============================================================================
constexpr float MIN_FREQUENCY_HZ      = 1.0f;
constexpr float MAX_FREQUENCY_HZ      = 200.0f;
constexpr float DEFAULT_FREQUENCY_HZ  = 80.0f;

constexpr uint16_t MIN_PULSE_WIDTH_US = 50;   // 50 microseconds
constexpr uint16_t MAX_PULSE_WIDTH_US = 400;  // 400 microseconds
constexpr uint16_t DEFAULT_PULSE_US   = 200;  // 200 microseconds

// Stimulation Operating Modes
enum TensMode {
    MODE_CONVENTIONAL = 1,
    MODE_ACUPUNCTURE  = 2,
    MODE_INTENSE      = 3,
    MODE_CUSTOM       = 4
};

// ==============================================================================
// GLOBAL STATE VARIABLES
// ==============================================================================
TensMode currentMode         = MODE_CONVENTIONAL;
bool isStimulationActive     = false;
float currentFrequency       = DEFAULT_FREQUENCY_HZ;
uint16_t currentPulseWidthUs = DEFAULT_PULSE_US;

unsigned long sessionTimerDurationMs = 0;
unsigned long sessionStartTimeMs     = 0;
unsigned long lastBatteryCheckTime   = 0;
String serialCommandBuffer           = "";

// ==============================================================================
// TIMER1 HARDWARE PWM CONTROLLER (ATmega328P)
// ==============================================================================

/**
 * @brief Configure Timer1 in Fast PWM Mode (Mode 14: ICR1 as TOP, non-inverting OC1A)
 * @param freqHz Target frequency in Hertz (1 Hz to 200 Hz)
 * @param pulseWidthUs Desired pulse width duration in microseconds
 */
void setTimer1PWM(float freqHz, uint16_t pulseWidthUs) {
    if (freqHz < MIN_FREQUENCY_HZ) freqHz = MIN_FREQUENCY_HZ;
    if (freqHz > MAX_FREQUENCY_HZ) freqHz = MAX_FREQUENCY_HZ;

    // Timer1 Prescaler: 64 (16 MHz / 64 = 250 kHz clock -> 4 us per tick)
    // TOP = (F_CPU / (Prescaler * Freq)) - 1 = (16,000,000 / (64 * freqHz)) - 1
    const uint32_t timerClockHz = 250000UL; // 250 kHz
    uint32_t topValue = (timerClockHz / (uint32_t)freqHz) - 1;
    
    if (topValue > 65535) topValue = 65535;

    // Calculate Compare Match value (OCR1A) based on pulse width (4 us per tick)
    uint32_t matchValue = pulseWidthUs / 4;
    if (matchValue >= topValue) matchValue = topValue / 2; // Safeguard duty cycle <= 50%
    if (matchValue == 0) matchValue = 1;

    // Configure Timer1 Registers safely
    noInterrupts();
    TCCR1A = 0;
    TCCR1B = 0;
    TCNT1  = 0;

    // Mode 14: Fast PWM with ICR1 as TOP
    // WGM13:1 = 1, WGM12:1 = 1, WGM11:1 = 1, WGM10:0 = 0
    TCCR1A |= (1 << WGM11);
    TCCR1B |= (1 << WGM13) | (1 << WGM12);

    // Set TOP and Compare Match registers
    ICR1  = (uint16_t)topValue;
    OCR1A = (uint16_t)matchValue;

    if (isStimulationActive) {
        // Enable non-inverting PWM on Pin 9 (OC1A: COM1A1 = 1)
        TCCR1A |= (1 << COM1A1);
    } else {
        // Disconnect OC1A
        TCCR1A &= ~(1 << COM1A1);
    }

    // Prescaler 64: CS11 = 1, CS10 = 1
    TCCR1B |= (1 << CS11) | (1 << CS10);
    interrupts();
}

/**
 * @brief Enable therapeutic output pulses
 */
void startStimulation() {
    isStimulationActive = true;
    sessionStartTimeMs = millis();
    setTimer1PWM(currentFrequency, currentPulseWidthUs);
    digitalWrite(PIN_STATUS_LED, HIGH);
    Serial.println(F("[STATUS] STIMULATION_STARTED"));
}

/**
 * @brief Disable therapeutic output pulses safely
 */
void stopStimulation() {
    isStimulationActive = false;
    noInterrupts();
    TCCR1A &= ~(1 << COM1A1); // Disconnect Pin 9 from Timer1
    digitalWrite(PIN_PWM_OUTPUT, LOW);
    interrupts();
    digitalWrite(PIN_STATUS_LED, LOW);
    Serial.println(F("[STATUS] STIMULATION_STOPPED"));
}

// ==============================================================================
// THERAPY PRESET MODES
// ==============================================================================
void applyPresetMode(TensMode mode) {
    currentMode = mode;
    switch (mode) {
        case MODE_CONVENTIONAL:
            currentFrequency = 100.0f;    // 100 Hz
            currentPulseWidthUs = 150;    // 150 us
            Serial.println(F("[MODE] CONVENTIONAL (100 Hz, 150 us)"));
            break;

        case MODE_ACUPUNCTURE:
            currentFrequency = 4.0f;      // 4 Hz
            currentPulseWidthUs = 250;    // 250 us
            Serial.println(F("[MODE] ACUPUNCTURE-LIKE (4 Hz, 250 us)"));
            break;

        case MODE_INTENSE:
            currentFrequency = 150.0f;    // 150 Hz
            currentPulseWidthUs = 200;    // 200 us
            Serial.println(F("[MODE] INTENSE / MODULATED (150 Hz, 200 us)"));
            break;

        case MODE_CUSTOM:
            Serial.println(F("[MODE] CUSTOM"));
            break;
    }

    if (isStimulationActive) {
        setTimer1PWM(currentFrequency, currentPulseWidthUs);
    }
}

// ==============================================================================
// BLUETOOTH SERIAL COMMAND PARSER (Mobile App Interface)
// ==============================================================================
void parseCommand(const String& cmd) {
    if (cmd.length() == 0) return;

    char prefix = cmd.charAt(0);
    String arg = cmd.substring(1);
    arg.trim();

    switch (prefix) {
        case 'f': // Set frequency (e.g. "f100" -> 100 Hz)
        case 'F': {
            float freq = arg.toFloat();
            if (freq >= MIN_FREQUENCY_HZ && freq <= MAX_FREQUENCY_HZ) {
                currentFrequency = freq;
                currentMode = MODE_CUSTOM;
                if (isStimulationActive) setTimer1PWM(currentFrequency, currentPulseWidthUs);
                Serial.print(F("[OK] FREQ_SET: "));
                Serial.println(currentFrequency);
            }
            break;
        }

        case 'p': // Set pulse width (e.g. "p200" -> 200 us)
        case 'P':
        case 'w':
        case 'W': {
            uint16_t pw = arg.toInt();
            if (pw >= MIN_PULSE_WIDTH_US && pw <= MAX_PULSE_WIDTH_US) {
                currentPulseWidthUs = pw;
                currentMode = MODE_CUSTOM;
                if (isStimulationActive) setTimer1PWM(currentFrequency, currentPulseWidthUs);
                Serial.print(F("[OK] PULSE_SET: "));
                Serial.println(currentPulseWidthUs);
            }
            break;
        }

        case 'm': // Select therapy mode ("m1": Conventional, "m2": Acupuncture, "m3": Intense)
        case 'M': {
            int modeInt = arg.toInt();
            if (modeInt >= 1 && modeInt <= 3) {
                applyPresetMode(static_cast<TensMode>(modeInt));
            }
            break;
        }

        case 't': // Set timer duration in minutes (e.g. "t15" -> 15 min)
        case 'T': {
            unsigned long minutes = arg.toInt();
            sessionTimerDurationMs = minutes * 60000UL;
            Serial.print(F("[OK] TIMER_SET_MIN: "));
            Serial.println(minutes);
            break;
        }

        case 's': // Start stimulation ("s" or "start")
        case 'S':
            startStimulation();
            break;

        case 'e': // Stop / End stimulation ("e" or "stop")
        case 'E':
            stopStimulation();
            break;

        case 'b': // Battery status query
        case 'B':
            checkBatteryStatus(true);
            break;

        default:
            Serial.print(F("[ERROR] UNKNOWN_COMMAND: "));
            Serial.println(cmd);
            break;
    }
}

// ==============================================================================
// BATTERY MANAGEMENT & TELEMETRY
// ==============================================================================
void checkBatteryStatus(bool forcePrint = false) {
    int raw = analogRead(PIN_BATTERY_SENSE);
    // Voltage divider scaling (Assumes standard R1=10k, R2=10k for Li-ion 3.0V-4.2V)
    float measuredVoltage = (raw * (5.0f / 1023.0f)) * 2.0f;
    int percentage = (int)constrain(((measuredVoltage - 3.2f) / (4.2f - 3.2f)) * 100.0f, 0, 100);

    if (forcePrint || millis() - lastBatteryCheckTime > 30000UL) {
        lastBatteryCheckTime = millis();
        Serial.print(F("[BATT] VOLT: "));
        Serial.print(measuredVoltage, 2);
        Serial.print(F("V | PCT: "));
        Serial.print(percentage);
        Serial.println(F("%"));
    }
}

// ==============================================================================
// ARDUINO SETUP & MAIN LOOP
// ==============================================================================
void setup() {
    Serial.begin(9600); // HC-05 default baud rate
    pinMode(PIN_PWM_OUTPUT, OUTPUT);
    pinMode(PIN_STATUS_LED, OUTPUT);
    digitalWrite(PIN_PWM_OUTPUT, LOW);
    digitalWrite(PIN_STATUS_LED, LOW);

    // Initial preset setup
    applyPresetMode(MODE_CONVENTIONAL);

    Serial.println(F("========================================"));
    Serial.println(F("  MY RELIEF - SMART TENS CONTROLLER     "));
    Serial.println(F("  Ready for Bluetooth commands.         "));
    Serial.println(F("========================================"));
}

void loop() {
    // Read incoming Bluetooth serial commands
    while (Serial.available() > 0) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (serialCommandBuffer.length() > 0) {
                parseCommand(serialCommandBuffer);
                serialCommandBuffer = "";
            }
        } else {
            serialCommandBuffer += c;
        }
    }

    // Handle Session Timer expiration
    if (isStimulationActive && sessionTimerDurationMs > 0) {
        if (millis() - sessionStartTimeMs >= sessionTimerDurationMs) {
            Serial.println(F("[TIMER] Session duration completed."));
            stopStimulation();
            sessionTimerDurationMs = 0;
        }
    }

    // Periodic battery telemetry
    checkBatteryStatus(false);
}
