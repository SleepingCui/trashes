// ReplayEngine.cpp
//
// ADoFaI Replay Engine
//
// Build:
//   MSVC:
//     cl /std:c++17 /O2 /EHsc ReplayEngine.cpp /link user32.lib
//
// Usage:
//   ReplayEngine.exe replay.json
//
// Optional:
//   ReplayEngine.exe replay.json -q
//
// Controls:
//   A       Start replay
//   Left    Decrease global offset by 1 ms
//   Right   Increase global offset by 1 ms
//   ESC     Stop replay
//
// Input sequence:
//   F -> G -> H -> J -> K -> F -> ...
//
// Notes:
//   - First offset is treated as trigger timing and is not replayed.
//   - Replay uses an absolute timeline.
//   - No phase correction is applied in this version.
//   - Timing is measured using QueryPerformanceCounter.
//

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <vector>


// ============================================================
// Configuration
// ============================================================

static constexpr double OFFSET_STEP_MS = 1.0;

// Busy wait threshold.
// Above this value we use Sleep(0) to reduce CPU usage.
// Below this value we busy-wait.
static constexpr double BUSY_WAIT_THRESHOLD_SEC = 0.002;


// ============================================================
// Win32 INPUT
// ============================================================

static constexpr DWORD INPUT_KEYBOARD_TYPE = INPUT_KEYBOARD;
static constexpr DWORD KEY_UP = KEYEVENTF_KEYUP;


// ============================================================
// High-resolution timer
// ============================================================

class HighResolutionTimer
{
public:
    HighResolutionTimer()
    {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
    }

    double now() const
    {
        LARGE_INTEGER current;
        QueryPerformanceCounter(&current);

        return static_cast<double>(
            current.QuadPart - start.QuadPart
        ) / static_cast<double>(frequency.QuadPart);
    }

private:
    LARGE_INTEGER frequency{};
    LARGE_INTEGER start{};
};


// ============================================================
// Statistics
// ============================================================

class Statistics
{
public:
    void add(double value)
    {
        values.push_back(value);
    }

    bool empty() const
    {
        return values.empty();
    }

    size_t size() const
    {
        return values.size();
    }

    double average() const
    {
        if (values.empty())
            return 0.0;

        double sum = 0.0;

        for (double value : values)
            sum += value;

        return sum / static_cast<double>(values.size());
    }

    double minimum() const
    {
        if (values.empty())
            return 0.0;

        return *std::min_element(
            values.begin(),
            values.end()
        );
    }

    double maximum() const
    {
        if (values.empty())
            return 0.0;

        return *std::max_element(
            values.begin(),
            values.end()
        );
    }

    double percentile(double p) const
    {
        if (values.empty())
            return 0.0;

        std::vector<double> sorted = values;

        std::sort(
            sorted.begin(),
            sorted.end()
        );

        double position =
            p * static_cast<double>(sorted.size() - 1);

        size_t lower =
            static_cast<size_t>(std::floor(position));

        size_t upper =
            static_cast<size_t>(std::ceil(position));

        if (lower == upper)
            return sorted[lower];

        double fraction =
            position - static_cast<double>(lower);

        return sorted[lower] +
               (sorted[upper] - sorted[lower]) *
               fraction;
    }

private:
    std::vector<double> values;
};


static void printStatistics(
    const char* name,
    const Statistics& stats
)
{
    if (stats.empty())
        return;

    std::cout
        << "  "
        << std::left
        << std::setw(18)
        << name
        << std::right
        << ": avg="
        << std::setw(10)
        << std::fixed
        << std::setprecision(4)
        << stats.average()
        << " ms  min="
        << std::setw(10)
        << stats.minimum()
        << " ms  max="
        << std::setw(10)
        << stats.maximum()
        << " ms  p95="
        << std::setw(10)
        << stats.percentile(0.95)
        << " ms  p99="
        << std::setw(10)
        << stats.percentile(0.99)
        << " ms"
        << std::endl;
}


// ============================================================
// Replay event
// ============================================================

struct ReplayEvent
{
    // Relative time from first note, seconds.
    double time = 0.0;

    WORD vk = 0;
};


// ============================================================
// JSON loader
//
// This intentionally only parses the "offsets" array.
// Expected structure:
//
// {
//     "offsets": [
//         [123.456, ...],
//         [456.789, ...],
//         ...
//     ]
// }
//
// The first number of every offset entry is used as timestamp.
// ============================================================

class ReplayJson
{
public:
    static bool load(
        const std::string& path,
        std::vector<double>& timestamps
    )
    {
        std::ifstream file(
            path,
            std::ios::binary
        );

        if (!file)
        {
            std::cerr
                << "Failed to open JSON file: "
                << path
                << std::endl;

            return false;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();

        std::string json = buffer.str();

        size_t offsetsPos =
            json.find("\"offsets\"");

        if (offsetsPos == std::string::npos)
        {
            offsetsPos =
                json.find("'offsets'");
        }

        if (offsetsPos == std::string::npos)
        {
            std::cerr
                << "JSON does not contain offsets."
                << std::endl;

            return false;
        }

        size_t arrayStart =
            json.find(
                '[',
                offsetsPos
            );

        if (arrayStart == std::string::npos)
        {
            std::cerr
                << "Invalid offsets array."
                << std::endl;

            return false;
        }

        // ----------------------------------------------------
        // Find the matching closing bracket of offsets.
        // ----------------------------------------------------

        size_t arrayEnd =
            findMatchingBracket(
                json,
                arrayStart
            );

        if (arrayEnd == std::string::npos)
        {
            std::cerr
                << "Could not find end of offsets array."
                << std::endl;

            return false;
        }

        // ----------------------------------------------------
        // Parse first number from each inner array.
        // ----------------------------------------------------

        size_t pos = arrayStart + 1;

        while (pos < arrayEnd)
        {
            // Skip whitespace.
            while (
                pos < arrayEnd &&
                std::isspace(
                    static_cast<unsigned char>(
                        json[pos]
                    )
                )
            )
            {
                ++pos;
            }

            if (pos >= arrayEnd)
                break;

            if (json[pos] != '[')
            {
                ++pos;
                continue;
            }

            size_t innerEnd =
                findMatchingBracket(
                    json,
                    pos
                );

            if (
                innerEnd == std::string::npos ||
                innerEnd > arrayEnd
            )
            {
                std::cerr
                    << "Invalid offsets entry."
                    << std::endl;

                return false;
            }

            size_t numberStart = pos + 1;

            while (
                numberStart < innerEnd &&
                std::isspace(
                    static_cast<unsigned char>(
                        json[numberStart]
                    )
                )
            )
            {
                ++numberStart;
            }

            if (numberStart >= innerEnd)
            {
                pos = innerEnd + 1;
                continue;
            }

            size_t numberEnd =
                numberStart;

            while (
                numberEnd < innerEnd &&
                json[numberEnd] != ',' &&
                json[numberEnd] != ']'
            )
            {
                ++numberEnd;
            }

            std::string number =
                json.substr(
                    numberStart,
                    numberEnd - numberStart
                );

            try
            {
                double timestamp =
                    std::stod(number);

                timestamps.push_back(
                    timestamp
                );
            }
            catch (...)
            {
                std::cerr
                    << "Failed to parse timestamp: "
                    << number
                    << std::endl;

                return false;
            }

            pos = innerEnd + 1;
        }

        if (timestamps.empty())
        {
            std::cerr
                << "No timestamps found."
                << std::endl;

            return false;
        }

        std::sort(
            timestamps.begin(),
            timestamps.end()
        );

        return true;
    }

private:
    static size_t findMatchingBracket(
        const std::string& text,
        size_t start
    )
    {
        if (
            start >= text.size() ||
            text[start] != '['
        )
        {
            return std::string::npos;
        }

        int depth = 0;

        bool inString = false;
        bool escape = false;

        for (
            size_t i = start;
            i < text.size();
            ++i
        )
        {
            char c = text[i];

            if (inString)
            {
                if (escape)
                {
                    escape = false;
                }
                else if (c == '\\')
                {
                    escape = true;
                }
                else if (c == '"')
                {
                    inString = false;
                }

                continue;
            }

            if (c == '"')
            {
                inString = true;
                continue;
            }

            if (c == '[')
            {
                ++depth;
            }
            else if (c == ']')
            {
                --depth;

                if (depth == 0)
                    return i;
            }
        }

        return std::string::npos;
    }
};


// ============================================================
// Keyboard input
// ============================================================

static INPUT makeKeyboardInput(
    WORD vk,
    bool keyUp
)
{
    INPUT input{};

    input.type = INPUT_KEYBOARD;

    input.ki.wVk = vk;
    input.ki.wScan = 0;

    input.ki.dwFlags =
        keyUp
            ? KEYEVENTF_KEYUP
            : 0;

    input.ki.time = 0;
    input.ki.dwExtraInfo = 0;

    return input;
}


static bool sendSingleKey(
    WORD vk,
    bool keyUp
)
{
    INPUT input =
        makeKeyboardInput(
            vk,
            keyUp
        );

    UINT result =
        SendInput(
            1,
            &input,
            sizeof(INPUT)
        );

    if (result != 1)
    {
        DWORD error =
            GetLastError();

        std::cerr
            << "SendInput failed: "
            << error
            << std::endl;

        return false;
    }

    return true;
}


static bool sendTransition(
    WORD oldVk,
    WORD newVk
)
{
    INPUT inputs[2]{};

    inputs[0] =
        makeKeyboardInput(
            oldVk,
            true
        );

    inputs[1] =
        makeKeyboardInput(
            newVk,
            false
        );

    UINT result =
        SendInput(
            2,
            inputs,
            sizeof(INPUT)
        );

    if (result != 2)
    {
        DWORD error =
            GetLastError();

        std::cerr
            << "SendInput failed: "
            << error
            << std::endl;

        return false;
    }

    return true;
}


// ============================================================
// Replay Engine
// ============================================================

class ReplayEngine
{
public:

    ReplayEngine(
        std::vector<ReplayEvent> events,
        bool quiet
    )
        : events(std::move(events)),
          quiet(quiet)
    {
    }

    void start()
    {
        if (running.exchange(true))
            return;

        stopped.store(false);

        worker =
            std::thread(
                &ReplayEngine::run,
                this
            );
    }

    void stop()
    {
        stopped.store(true);
    }

    void join()
    {
        if (
            worker.joinable()
        )
        {
            worker.join();
        }
    }

    void adjustOffset(
        double milliseconds
    )
    {
        std::lock_guard<std::mutex> lock(
            offsetMutex
        );

        offsetSeconds +=
            milliseconds / 1000.0;

        std::cout
            << "Global offset: "
            << std::showpos
            << std::fixed
            << std::setprecision(3)
            << offsetSeconds * 1000.0
            << " ms"
            << std::noshowpos
            << std::endl;
    }

private:

    // ========================================================
    // Wait until absolute target time.
    // ========================================================

    bool waitUntil(
        const HighResolutionTimer& timer,
        double target
    )
    {
        while (!stopped.load())
        {
            double now =
                timer.now();

            double remaining =
                target - now;

            if (remaining <= 0.0)
                return true;

            if (
                remaining >
                BUSY_WAIT_THRESHOLD_SEC
            )
            {
                // Yield the remainder of the quantum.
                Sleep(0);
            }
            else
            {
                // Final ~2ms:
                // pure busy wait.
                //
                // Do NOT call Sleep here.
                // Windows timer granularity is
                // too coarse for this workload.
                _mm_pause();
            }
        }

        return false;
    }


    double getOffset()
    {
        std::lock_guard<std::mutex> lock(
            offsetMutex
        );

        return offsetSeconds;
    }


    void printProgress(
        size_t count,
        size_t total,
        const Statistics& targetDelta,
        const Statistics& actualDelta,
        const Statistics& sendInput,
        const Statistics& rawLate,
        const Statistics& correctedLate,
        double offset
    )
    {
        if (quiet)
            return;

        std::cout
            << std::endl;

        std::cout
            << "[PROF] #"
            << count
            << " / "
            << total
            << std::endl;

        printStatistics(
            "target_delta",
            targetDelta
        );

        printStatistics(
            "actual_delta",
            actualDelta
        );

        printStatistics(
            "SendInput",
            sendInput
        );

        printStatistics(
            "raw_late",
            rawLate
        );

        printStatistics(
            "corrected_late",
            correctedLate
        );

        std::cout
            << "  "
            << std::left
            << std::setw(18)
            << "global offset"
            << std::right
            << ": "
            << std::showpos
            << std::fixed
            << std::setprecision(4)
            << offset * 1000.0
            << " ms"
            << std::noshowpos
            << std::endl;
    }


    void run()
    {
        HighResolutionTimer timer;

        Statistics targetDelta;
        Statistics actualDelta;
        Statistics sendInput;
        Statistics rawLate;
        Statistics correctedLate;

        double previousTarget =
            0.0;

        double previousActual =
            0.0;

        bool hasPrevious =
            false;

        WORD currentVk = 0;

        bool hasCurrentKey =
            false;

        size_t sentCount = 0;

        const size_t total =
            events.size();

        // ----------------------------------------------------
        // Absolute replay start.
        //
        // Event time 0 happens at timer.now() == 0.
        // ----------------------------------------------------

        const double replayStart =
            timer.now();

        for (
            size_t index = 0;
            index < events.size();
            ++index
        )
        {
            if (stopped.load())
                break;

            const ReplayEvent& event =
                events[index];

            const double offset =
                getOffset();

            // ------------------------------------------------
            // Original timeline.
            // ------------------------------------------------

            const double originalTarget =
                replayStart
                + event.time
                + offset;

            // No automatic phase correction.
            const double target =
                originalTarget;

            // ------------------------------------------------
            // Wait.
            // ------------------------------------------------

            if (
                !waitUntil(
                    timer,
                    target
                )
            )
            {
                break;
            }

            const double actualBefore =
                timer.now();

            // ------------------------------------------------
            // Profiling: delta.
            // ------------------------------------------------

            if (hasPrevious)
            {
                targetDelta.add(
                    (
                        target
                        - previousTarget
                    ) * 1000.0
                );

                actualDelta.add(
                    (
                        actualBefore
                        - previousActual
                    ) * 1000.0
                );
            }

            // ------------------------------------------------
            // Raw phase error.
            // ------------------------------------------------

            rawLate.add(
                (
                    actualBefore
                    - originalTarget
                ) * 1000.0
            );

            // Since there is no correction:
            // corrected late == raw late.
            correctedLate.add(
                (
                    actualBefore
                    - target
                ) * 1000.0
            );

            // ------------------------------------------------
            // Send key.
            // ------------------------------------------------

            const double sendStart =
                timer.now();

            bool success = false;

            if (!hasCurrentKey)
            {
                success =
                    sendSingleKey(
                        event.vk,
                        false
                    );
            }
            else
            {
                success =
                    sendTransition(
                        currentVk,
                        event.vk
                    );
            }

            const double sendEnd =
                timer.now();

            sendInput.add(
                (
                    sendEnd
                    - sendStart
                ) * 1000.0
            );

            if (!success)
            {
                stopped.store(true);
                break;
            }

            currentVk =
                event.vk;

            hasCurrentKey =
                true;

            previousTarget =
                target;

            previousActual =
                actualBefore;

            hasPrevious =
                true;

            ++sentCount;

            // ------------------------------------------------
            // Every 1000 events.
            // ------------------------------------------------

            if (
                sentCount % 1000 == 0
            )
            {
                printProgress(
                    sentCount,
                    total,
                    targetDelta,
                    actualDelta,
                    sendInput,
                    rawLate,
                    correctedLate,
                    offset
                );
            }
        }

        // ====================================================
        // Release currently held key.
        // ====================================================

        if (hasCurrentKey)
        {
            sendSingleKey(
                currentVk,
                true
            );
        }

        running.store(false);

        // ====================================================
        // Final statistics.
        // ====================================================

        std::cout
            << std::endl;

        if (stopped.load())
        {
            std::cout
                << "Replay stopped."
                << std::endl;
        }
        else
        {
            std::cout
                << "Replay completed."
                << std::endl;
        }

        std::cout
            << std::endl;

        std::cout
            << "========== Profiling =========="
            << std::endl;

        printStatistics(
            "target_delta",
            targetDelta
        );

        printStatistics(
            "actual_delta",
            actualDelta
        );

        printStatistics(
            "SendInput",
            sendInput
        );

        printStatistics(
            "raw_late",
            rawLate
        );

        printStatistics(
            "corrected_late",
            correctedLate
        );

        std::cout
            << "================================"
            << std::endl;

        std::cout
            << std::endl;

        std::cout
            << "Actual sends: "
            << sentCount
            << " / "
            << total
            << std::endl;
    }

private:

    std::vector<ReplayEvent> events;

    bool quiet = false;

    std::atomic<bool> running{
        false
    };

    std::atomic<bool> stopped{
        false
    };

    std::thread worker;

    std::mutex offsetMutex;

    double offsetSeconds = 0.0;
};


// ============================================================
// Key mapping
// ============================================================

static WORD keyToVk(
    char key
)
{
    switch (
        static_cast<char>(
            std::tolower(
                static_cast<unsigned char>(
                    key
                )
            )
        )
    )
    {
    case 'f':
        return 0x46;

    case 'g':
        return 0x47;

    case 'h':
        return 0x48;

    case 'j':
        return 0x4A;

    case 'k':
        return 0x4B;

    default:
        return 0;
    }
}


// ============================================================
// Global keyboard listener
//
// GetAsyncKeyState polling is used instead of a third-party
// keyboard library.
//
// This also means the entire program is native Win32.
// ============================================================

class KeyboardController
{
public:

    explicit KeyboardController(
        ReplayEngine& engine
    )
        : engine(engine)
    {
    }

    void run()
    {
        bool previousA =
            false;

        bool previousLeft =
            false;

        bool previousRight =
            false;

        while (!shouldExit)
        {
            bool a =
                isPressed(
                    'A'
                );

            bool left =
                isPressed(
                    VK_LEFT
                );

            bool right =
                isPressed(
                    VK_RIGHT
                );

            bool esc =
                isPressed(
                    VK_ESCAPE
                );

            // ------------------------------------------------
            // A: trigger.
            // ------------------------------------------------

            if (
                a &&
                !previousA &&
                !started
            )
            {
                started = true;

                std::cout
                    << "A detected. "
                       "Starting replay..."
                    << std::endl;

                engine.start();
            }

            // ------------------------------------------------
            // Left.
            // ------------------------------------------------

            if (
                left &&
                !previousLeft &&
                started
            )
            {
                engine.adjustOffset(
                    -OFFSET_STEP_MS
                );
            }

            // ------------------------------------------------
            // Right.
            // ------------------------------------------------

            if (
                right &&
                !previousRight &&
                started
            )
            {
                engine.adjustOffset(
                    OFFSET_STEP_MS
                );
            }

            // ------------------------------------------------
            // ESC.
            // ------------------------------------------------

            if (esc)
            {
                engine.stop();

                shouldExit = true;

                break;
            }

            previousA =
                a;

            previousLeft =
                left;

            previousRight =
                right;

            // Poll at ~1kHz.
            Sleep(1);
        }
    }

private:

    static bool isPressed(
        int vk
    )
    {
        return (
            GetAsyncKeyState(vk)
            & 0x8000
        ) != 0;
    }

private:

    ReplayEngine& engine;

    bool started = false;

    bool shouldExit = false;
};


// ============================================================
// Main
// ============================================================

int main(
    int argc,
    char* argv[]
)
{
    std::cout
        << "========================================"
        << std::endl;

    std::cout
        << "       ADoFaI ReplayEngine"
        << std::endl;

    std::cout
        << "========================================"
        << std::endl;

    // --------------------------------------------------------
    // Arguments
    // --------------------------------------------------------

    if (argc < 2)
    {
        std::cout
            << "Usage:"
            << std::endl;

        std::cout
            << "  ReplayEngine.exe replay.json"
            << std::endl;

        std::cout
            << "  ReplayEngine.exe replay.json -q"
            << std::endl;

        return 1;
    }

    const std::string jsonPath =
        argv[1];

    bool quiet = false;

    for (
        int i = 2;
        i < argc;
        ++i
    )
    {
        if (
            std::string(argv[i])
            == "-q"
        )
        {
            quiet = true;
        }
    }

    // --------------------------------------------------------
    // Load JSON
    // --------------------------------------------------------

    std::cout
        << "File: "
        << jsonPath
        << std::endl;

    std::vector<double> timestamps;

    if (
        !ReplayJson::load(
            jsonPath,
            timestamps
        )
    )
    {
        return 1;
    }

    if (timestamps.size() < 2)
    {
        std::cerr
            << "Not enough timestamps."
            << std::endl;

        return 1;
    }

    // --------------------------------------------------------
    // First timestamp = trigger.
    // --------------------------------------------------------

    const double baseTimestamp =
        timestamps.front();

    std::vector<ReplayEvent> events;

    events.reserve(
        timestamps.size() - 1
    );

    const char keySequence[] = {
        'f',
        'g',
        'h',
        'j',
        'k'
    };

    for (
        size_t i = 1;
        i < timestamps.size();
        ++i
    )
    {
        ReplayEvent event;

        // Existing Python code divides timestamps by 1000.
        //
        // Here timestamps are assumed to be milliseconds.
        event.time =
            (
                timestamps[i]
                - baseTimestamp
            ) / 1000.0;

        event.vk =
            keyToVk(
                keySequence[
                    (i - 1) % 5
                ]
            );

        events.push_back(
            event
        );
    }

    // --------------------------------------------------------
    // Information.
    // --------------------------------------------------------

    std::cout
        << "Total timestamps: "
        << timestamps.size()
        << std::endl;

    std::cout
        << "Replay events: "
        << events.size()
        << std::endl;

    std::cout
        << "Keys: F G H J K"
        << std::endl;

    if (!events.empty())
    {
        std::cout
            << "Duration: "
            << std::fixed
            << std::setprecision(3)
            << events.back().time
            << " s"
            << std::endl;
    }

    std::cout
        << "Global offset step: "
        << OFFSET_STEP_MS
        << " ms"
        << std::endl;

    std::cout
        << std::endl;

    // --------------------------------------------------------
    // Timer resolution information.
    // --------------------------------------------------------

    LARGE_INTEGER frequency;

    QueryPerformanceFrequency(
        &frequency
    );

    std::cout
        << "QPC frequency: "
        << frequency.QuadPart
        << " Hz"
        << std::endl;

    // --------------------------------------------------------
    // Replay engine.
    // --------------------------------------------------------

    ReplayEngine engine(
        std::move(events),
        quiet
    );

    // --------------------------------------------------------
    // Keyboard controller.
    // --------------------------------------------------------

    std::cout
        << std::endl;

    std::cout
        << "Waiting for A..."
        << std::endl;

    std::cout
        << "A     Start"
        << std::endl;

    std::cout
        << "Left  Offset -1 ms"
        << std::endl;

    std::cout
        << "Right Offset +1 ms"
        << std::endl;

    std::cout
        << "ESC   Stop"
        << std::endl;

    std::cout
        << std::endl;

    KeyboardController keyboard(
        engine
    );

    keyboard.run();

    engine.stop();

    engine.join();

    return 0;
}