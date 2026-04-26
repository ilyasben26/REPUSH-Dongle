#include <Arduino.h>
#include <Ed25519.h>
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

  Serial.println("REPUSH Dongle Initialized.");
  Serial.println("Commands:");
  Serial.println("  led_on / led_off");
  Serial.println("  debug_on / debug_off");
  Serial.println("  find_valid");
  Serial.println("  reconfigure <state_index>");
  Serial.println("  keygen <challenge> <state_index>");
  Serial.println("  sign <challenge> <state_index> <nonce> <cookies_b64>");
  Serial.println("  challenge <c> <state_index> <count> <delay>");
  Serial.println("    state_index: 0-10");
  Serial.println("  challenge choice-puf <tc> <tt> <bc> <bt> <count> <delay>");
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
    else if (command_str.startsWith("keygen "))
    {
      int first_space = command_str.indexOf(' ', 7);

      if (first_space != -1)
      {
        int challenge = command_str.substring(7, first_space).toInt();
        int state_index = command_str.substring(first_space + 1).toInt();

        if (state_index < 0 || state_index > 10)
        {
          Serial.println("Error: state_index must be between 0 and 10.");
        }
        else
        {
          std::array<uint8_t, 32> priv_key_array = challenge_lr_puf(challenge, state_index, 1, 1);
          uint8_t privateKey[32];
          std::copy(priv_key_array.begin(), priv_key_array.end(), privateKey);

          uint8_t publicKey[32];
          Ed25519::derivePublicKey(publicKey, privateKey);

          Serial.print("PublicKey: ");
          for (int i = 0; i < 32; i++)
          {
            if (publicKey[i] < 16)
              Serial.print("0");
            Serial.print(publicKey[i], HEX);
          }
          Serial.println();
        }
      }
      else
      {
        Serial.println("Error: Invalid command format.");
        Serial.println("Expected: keygen <challenge> <state_index>");
      }
    }
    else if (command_str.startsWith("sign "))
    {
      int first_space = command_str.indexOf(' ', 5);
      int second_space = command_str.indexOf(' ', first_space + 1);
      int third_space = command_str.indexOf(' ', second_space + 1);

      if (first_space != -1 && second_space != -1 && third_space != -1)
      {
        int challenge = command_str.substring(5, first_space).toInt();
        int state_index = command_str.substring(first_space + 1, second_space).toInt();
        String nonce = command_str.substring(second_space + 1, third_space);
        String cookies_b64 = command_str.substring(third_space + 1);

        String payload_str = nonce + cookies_b64;

        if (state_index < 0 || state_index > 10)
        {
          Serial.println("Error: state_index must be between 0 and 10.");
        }
        else
        {
          std::array<uint8_t, 32> priv_key_array = challenge_lr_puf(challenge, state_index, 1, 1);
          uint8_t privateKey[32];
          std::copy(priv_key_array.begin(), priv_key_array.end(), privateKey);

          uint8_t publicKey[32];
          Ed25519::derivePublicKey(publicKey, privateKey);

          uint8_t signature[64];
          Ed25519::sign(signature, privateKey, publicKey, payload_str.c_str(), payload_str.length());

          Serial.print("Signature: ");
          for (int i = 0; i < 64; i++)
          {
            if (signature[i] < 16)
              Serial.print("0");
            Serial.print(signature[i], HEX);
          }
          Serial.println();
        }
      }
      else
      {
        Serial.println("Error: Invalid command format.");
        Serial.println("Expected: sign <challenge> <state_index> <nonce> <cookies_b64>");
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
        std::array<uint8_t, 16> majority_response;
        execute_challenge(top_tune, bottom_tune, top_choice, bottom_choice, count, resp_delay_ms, majority_response);
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
        Serial.println("Expected: challenge <c> <state_index> <count> <delay>");
      }
    }
    else if (command_str.equalsIgnoreCase("rc"))
    {
      Serial.println("Sending 'rc' to FPGA...");

      // Flush any pending data from the FPGA (e.g. earlier boot messages)
      while (Serial1.available())
      {
        Serial1.read();
      }

      Serial1.print("rc\r\n");

      Serial.print("FPGA Response:\n");
      // Read everything that comes back with a small timeout
      unsigned long start_time = millis();
      while (millis() - start_time < 500)
      {
        while (Serial1.available())
        {
          Serial.print((char)Serial1.read());
          start_time = millis(); // Reset timeout if data is flowing
        }
      }
      Serial.println();
    }
    else if (command_str.length() > 0)
    {
      Serial.print("Unknown command: '");
      Serial.print(command_str);
      Serial.println("'");
    }
  }
}
