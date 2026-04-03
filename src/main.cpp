#include <Arduino.h>
#include "puf_functions.h"

int led = LED_BUILTIN;

struct Challenge
{
  int tc;
  int tt;
  int bc;
  int bt;
};

Challenge *valid_challenges = nullptr;
int num_valid_challenges = 0;
int capacity = 0;

bool debug_mode = false;

void find_valid_challenges()
{
  Serial.println("[LR-PUF] Finding valid challenges...");

  unsigned long start_time = millis();

  if (valid_challenges != nullptr)
  {
    delete[] valid_challenges;
    valid_challenges = nullptr;
  }

  num_valid_challenges = 0;
  capacity = 10;
  valid_challenges = new Challenge[capacity];

  int response_delay = 1;
  const long ALL_ONES_30_BIT = 0x3FFFFFFF;

  for (int tc = 1; tc <= 3; tc++)
  {
    for (int tt = 0; tt <= 7; tt++)
    {
      for (int bc = 0; bc <= 2; bc++)
      {
        if (tc > bc)
        {
          for (int bt = 0; bt <= 7; bt++)
          {
            long puf_response = execute_challenge(tt, bt, tc, bc, 1, response_delay);

            if (puf_response != ALL_ONES_30_BIT)
            {
              // If the array is full, double its capacity
              if (num_valid_challenges >= capacity)
              {
                int new_capacity = capacity * 2;
                Challenge *new_array = new Challenge[new_capacity];
                for (int i = 0; i < num_valid_challenges; i++)
                {
                  new_array[i] = valid_challenges[i];
                }
                delete[] valid_challenges;
                valid_challenges = new_array;
                capacity = new_capacity;
              }

              valid_challenges[num_valid_challenges] = {tc, tt, bc, bt};
              num_valid_challenges++;
            }
          }
        }
      }
    }
  }

  Serial.print("[LR-PUF] Total valid challenges found and stored: ");
  Serial.println(num_valid_challenges);
  Serial.print("[LR-PUF] Final capacity of valid challenges array: ");
  Serial.println(capacity);

  unsigned long elapsed_time = millis() - start_time;
  Serial.print("[LR-PUF] Valid challenges lookup time: ");
  Serial.print(elapsed_time / 1000.0, 3);
  Serial.println(" s");

  for (int i = 0; i < num_valid_challenges; i++)
  {
    Serial.print("Stored challenge ");
    Serial.print(i);
    Serial.print(": tc=");
    Serial.print(valid_challenges[i].tc);
    Serial.print(", tt=");
    Serial.print(valid_challenges[i].tt);
    Serial.print(", bc=");
    Serial.print(valid_challenges[i].bc);
    Serial.print(", bt=");
    Serial.println(valid_challenges[i].bt);
  }
}

void setup()
{
  Serial.begin(115200);  // Computer <-> Arduino
  Serial1.begin(115200); // Arduino <-> FPGA
  pinMode(led, OUTPUT);

  while (!Serial)
  {
    delay(10);
  }

  // Initialize valid configurations (example values, adjust as needed)

  Serial.println("Arduino PUF Client Initialized.");
  Serial.println("Commands:");
  Serial.println("  led_on / led_off");
  Serial.println("  debug_on / debug_off");
  Serial.println("  find_valid");
  Serial.println("  challenge <tc> <tt> <bc> <bt> <count> <delay>");
  Serial.println("    tt: top_tune (0-7), bt: bottom_tune (0-7)");
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
    else if (command_str.startsWith("challenge"))
    {
      int args[6];
      int arg_count = 0;
      int current_pos = command_str.indexOf(' ');

      while (current_pos != -1 && arg_count < 6)
      {
        int next_pos = command_str.indexOf(' ', current_pos + 1);
        String arg_str = (next_pos == -1) ? command_str.substring(current_pos + 1) : command_str.substring(current_pos + 1, next_pos);
        args[arg_count++] = arg_str.toInt();
        current_pos = next_pos;
      }

      if (arg_count >= 5)
      {
        int top_choice = args[0];
        int top_tune = args[1];
        int bottom_choice = args[2];
        int bottom_tune = args[3];
        int count = args[4];
        int resp_delay_ms = (arg_count == 6) ? args[5] : 50;

        execute_challenge(top_tune, bottom_tune, top_choice, bottom_choice, count, resp_delay_ms);
      }
      else
      {
        Serial.println("Error: Invalid 'challenge' command format.");
        Serial.println("Expected: challenge <tc> <tt> <bc> <bt> <count> [delay]");
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
