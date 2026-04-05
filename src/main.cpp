#include <Arduino.h>
#include "puf_functions.h"

int led = LED_BUILTIN;

bool debug_mode = false;

void setup()
{
  Serial.begin(115200);  // Computer <-> Arduino
  Serial1.begin(115200); // Arduino <-> FPGA
  pinMode(led, OUTPUT);

  while (!Serial)
  {
    delay(10);
  }

  Serial.println("Arduino PUF Client Initialized.");
  Serial.println("Commands:");
  Serial.println("  led_on / led_off");
  Serial.println("  debug_on / debug_off");
  Serial.println("  find_valid");
  Serial.println("  reconfigure <state_index>");
  Serial.println("  challenge <c> <state_index> <count> <delay>");
  Serial.println("    state_index: 0-10");
  Serial.println("  challenge choice-puf <tc> <tt> <bc> <bt> <count> <delay>");
  Serial.println("    tt: top_tune (0-31), bt: bottom_tune (0-31)");
  Serial.println("    tc: top_choice (1-3), bc: bottom_choice (0-2)");
  Serial.println("    count: number of reads (e.g., 100)");
  Serial.println("    delay: response delay in ms (e.g., 50)");
}

void loop()
{
  if (Serial.available())
  {
    String command_str = Serial.readStringUntil('\n');
    command_str.trim();

    Serial.print("**** Received command: '");
    Serial.print(command_str);
    Serial.println("' ****");

    if (command_str.equalsIgnoreCase("led_on"))
    {
      digitalWrite(led, HIGH);
      Serial.println("LED ON");
    }
    else if (command_str.equalsIgnoreCase("led_off"))
    {
      digitalWrite(led, LOW);
      Serial.println("LED OFF");
    }
    else if (command_str.equalsIgnoreCase("debug_on"))
    {
      debug_mode = true;
      Serial.println("Debug mode ON");
    }
    else if (command_str.equalsIgnoreCase("debug_off"))
    {
      debug_mode = false;
      Serial.println("Debug mode OFF");
    }
    else if (command_str.startsWith("find_valid"))
    {
      find_valid_challenges();
    }
    else if (command_str.startsWith("reconfigure"))
    {
      int args[1];
      int arg_count = 0;
      int current_pos = command_str.indexOf(' ');

      while (current_pos != -1 && arg_count < 1)
      {
        int next_pos = command_str.indexOf(' ', current_pos + 1);
        String arg_str = (next_pos == -1) ? command_str.substring(current_pos + 1) : command_str.substring(current_pos + 1, next_pos);
        args[arg_count++] = arg_str.toInt();
        current_pos = next_pos;
      }

      if (arg_count == 1)
      {
        int state_index = args[0];

        if (state_index < 0 || state_index > 10)
        {
          Serial.println("Error: state_index must be between 0 and 10.");
        }
        else
        {
          reconfigure(state_index);
        }
      }
      else
      {
        Serial.println("Error: Invalid command format.");
        Serial.println("Expected: reconfigure <state_index>");
      }
    }
    else if (command_str.startsWith("challenge choice-puf "))
    {
      String args_str = command_str.substring(String("challenge choice-puf ").length());
      int top_choice = 0;
      int top_tune = 0;
      int bottom_choice = 0;
      int bottom_tune = 0;
      int count = 0;
      int resp_delay_ms = 50;

      int parsed = sscanf(args_str.c_str(), "%d %d %d %d %d %d",
                          &top_choice, &top_tune, &bottom_choice, &bottom_tune,
                          &count, &resp_delay_ms);

      if (parsed >= 5)
      {
        execute_challenge(top_tune, bottom_tune, top_choice, bottom_choice, count, resp_delay_ms);
      }
      else
      {
        Serial.println("Error: Invalid 'challenge' command format.");
        Serial.println("Expected: challenge choice-puf <tc> <tt> <bc> <bt> <count> [delay]");
      }
    }
    else if (command_str.startsWith("challenge "))
    {
      int args[4];
      int arg_count = 0;
      int current_pos = command_str.indexOf(' ');

      while (current_pos != -1 && arg_count < 4)
      {
        int next_pos = command_str.indexOf(' ', current_pos + 1);
        String arg_str = (next_pos == -1) ? command_str.substring(current_pos + 1) : command_str.substring(current_pos + 1, next_pos);
        args[arg_count++] = arg_str.toInt();
        current_pos = next_pos;
      }

      if (arg_count == 4)
      {
        int challenge = args[0];
        int state_index = args[1];
        int count = args[2];
        int resp_delay_ms = args[3];

        if (state_index < 0 || state_index > 10)
        {
          Serial.println("Error: state_index must be between 0 and 10.");
        }
        else
        {
          challenge_lr_puf(challenge, state_index, count, resp_delay_ms);
        }
      }
      else
      {
        Serial.println("Error: Invalid command format.");
        Serial.println("Expected: challenge <c> <state_index>");
      }
    }
    else if (command_str.length() > 0)
    {
      Serial.print("Unknown command: '");
      Serial.print(command_str);
      Serial.println("'");
    }
  }
}
