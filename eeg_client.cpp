#include "central_sender.cpp"
#include "unicorn.h"

const double RATE  = UNICORN_SAMPLING_RATE;   // 250 Hz
const int    BLOCK = 1;                       // change this to get differnt packets per second

const std::string SERVER_HOST = "192.168.1.10"; // random for now bc idk the cema windows ip
const std::string SERVER_PORT = "9125";
const char* UNICORN_SERIAL = "UN-2021.05.36"; //This is a fake serial number we just need the number on our device

// Just printing out manufcturer error in lieu of an error number
void check(int error) {
    if (error != UNICORN_ERROR_SUCCESS) throw std::runtime_error(UNICORN_GetLastErrorText());
}

int main(int argc, char** argv) {
    ServerLink link{SERVER_HOST, SERVER_PORT, "UnicornEEG", UNICORN_TOTAL_CHANNELS_COUNT, RATE, FORMAT_FLOAT32};
    
    while (true) {
        UNICORN_HANDLE handle = 0;
        try {
            // Open paired device
            check(UNICORN_OpenDevice(UNICORN_SERIAL, &handle));
            
            // Get number of values per scan (channels)
            uint32_t channels = 0;   // 17: 8 EEG, 3 accel, 3 gyro, battery, counter, validation
            check(UNICORN_GetNumberOfAcquiredChannels(handle, &channels));
            link.channels = channels;   // tell the server what the headset actually reports

            // Turns the EEG headset on 
            check(UNICORN_StartAcquisition(handle, FALSE));   // FALSE = real signal, not test signal (from the unicorn API, toggle to change between test and real signal)

            // initializing a vector of one block of data to be sent over the wire (number of scans x channels per scan)
            // where a scan is one singular snapshot of all values across teh EEG at a moment in time
            std::vector<float> block(BLOCK * channels);
            
            while (true) {
                // get data block off the stream
                check(UNICORN_GetData(handle, BLOCK, block.data(), block.size()));   
                const double t0 = now_seconds() - (BLOCK - 1) / RATE;        // timestamp
                link.send(t0, block.data(), block.size() * sizeof(float));
            }
        } catch (const std::exception& e) {
           
            //printing to the error stream and retrying upon error
            std::cerr << "Unicorn: " << e.what() << ", retrying\n";
            //clean up buffer & free bluetooth connection upon dropped signal
            if (handle != 0) UNICORN_CloseDevice(&handle);
            // time buffer between retry attempts
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}
