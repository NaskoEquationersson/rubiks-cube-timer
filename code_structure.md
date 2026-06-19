

# Sections

- Timer Part
- Receiver Display

# Timer Code

Define port numbers.

Define functions that would be used in the code here.

Start

Connect to WiFi, then ESP-NOW.

Define all i/o ports if needed.

Define all variables that would be needed in the code here.

Reset all LEDs to LOW.

While loop to check if the RESET button is not pressed.
Do the RESET function otherwise.

If the RESET button is not pressed, execute the check for the press.

If at least one of the touch sensors are pressed, begin startup sequence.
First set RED_LED to HIGH, then set it LOW, and GREEN_LED to HIGH.
Make sure to do the timing right.

If any point in that code, the reset button is pressed, or no touch sensor is on,
close the loop and close all the lights with the RESET function.

If the sensors are pressed the required time, wait for one of them to be removed, and start the time.

Send this data to the other timer using ESP-NOW.

Wait for all sensors to activate afterwards, after the time starts.
Make sure to accept RESET function anytime.

If all of the sensors are pressed again, stop the time, and send the signal to the remote.

After the time is complete, wait for another entry with either RESET button, or all sensors pressed again.

# Receiver Code

Define port numbers.

Define functions that would be used in the code here.

Start

Connect to WiFi, then ESP-NOW.

Define all i/o ports if needed.

Define all variables that would be needed in the code here.

Receive the data from the other ESP32 via ESP-NOW, and write it on the LCD screen.

If RESET signal comes, empty the LCD.

If FINISH signal comes, open and close the LCD with the time.

Do all of these in a while loop.