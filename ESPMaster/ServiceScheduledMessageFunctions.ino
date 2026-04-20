//Replace/add a scheduled message and persist the result afterwards
void addAndPersistScheduledMessage(String scheduledText, long scheduledDateTimeUnix, bool showIndefinitely) {
  addScheduledMessage(scheduledText, scheduledDateTimeUnix, showIndefinitely);
  writeScheduledMessagesToFile();
}

//Replace/add a scheduled message
void addScheduledMessage(String scheduledText, long scheduledDateTimeUnix, bool showIndefinitely) {
  SerialPrintln("Processing New Scheduled Message");
  SerialPrintln("-- Message: " + scheduledText);
  SerialPrint("-- Unix: ");
  SerialPrintln(scheduledDateTimeUnix);
  SerialPrint("-- Show Indefinitely: ");
  SerialPrintln(showIndefinitely ? "Yes" : "No");

  //Find the existing scheduled message and update it if one exists
  for(int scheduledMessageIndex = 0; scheduledMessageIndex < scheduledMessages.size(); scheduledMessageIndex++) {
    ScheduledMessage scheduledMessage = scheduledMessages[scheduledMessageIndex];

    if (scheduledDateTimeUnix == scheduledMessage.ScheduledDateTimeUnix) {
      SerialPrintln("Removing Existing Scheduled Message due to be shown, it will be replaced");
      scheduledMessages.erase(scheduledMessages.begin() + scheduledMessageIndex);
      break;
    }
  }

  //Add the new scheduled message with the new value
  if (scheduledDateTimeUnix > (long)time(nullptr)) {
    SerialPrintln("Adding new Scheduled Message");
    scheduledMessages.push_back({scheduledText, scheduledDateTimeUnix, showIndefinitely});
  }
  else {
    SerialPrintln("Not adding Scheduled Message as it is in the past");
  }
}

//Remove and delete a existing scheduled message if found
bool removeScheduledMessage(long scheduledDateTimeUnix) {
  //Find the existing scheduled message and delete it
  for(int scheduledMessageIndex = 0; scheduledMessageIndex < scheduledMessages.size(); scheduledMessageIndex++) {
    ScheduledMessage scheduledMessage = scheduledMessages[scheduledMessageIndex];

    if (scheduledDateTimeUnix == scheduledMessage.ScheduledDateTimeUnix) {
      SerialPrintln("Deleting Scheduled Message due to be shown: " + scheduledMessage.Message);
      scheduledMessages.erase(scheduledMessages.begin() + scheduledMessageIndex);

      writeScheduledMessagesToFile();

      return true;
    }
  }
  
  return false;
}

//Check if scheduled message is due to be shown
void checkScheduledMessages() {   
  //Iterate over the current bunch of scheduled messages. If we find one where the current time exceeds when we should show
  //the message, then we need to show that message immediately
  //`ScheduledDateTimeUnix` is declared `long` in Classes.h; match the type to
  //avoid signed/unsigned comparison surprises.
  long currentTimeUnix = (long)time(nullptr);
  for(int scheduledMessageIndex = 0; scheduledMessageIndex < scheduledMessages.size(); scheduledMessageIndex++) {
    ScheduledMessage scheduledMessage = scheduledMessages[scheduledMessageIndex];

    if (currentTimeUnix > scheduledMessage.ScheduledDateTimeUnix) {
      SerialPrintln("Scheduled Message due to be shown: " + scheduledMessage.Message);
      SerialPrint("Scheduled Message to be Shown Indefinitely: ");
      SerialPrintln(scheduledMessage.ShowIndefinitely ? "Yes" : "No");

      if (scheduledMessage.ShowIndefinitely) {
        deviceMode = DEVICE_MODE_TEXT;
        inputText = scheduledMessage.Message;
        showText(scheduledMessage.Message);
      }
      else {
        showText(scheduledMessage.Message, scheduledMessageDisplayTimeMillis);
      }      

      scheduledMessages.erase(scheduledMessages.begin() + scheduledMessageIndex);
      writeScheduledMessagesToFile();
      break;
    }
  }
}

//Parse JSON scheduled messages into the current known scheduled messages
void readScheduledMessagesFromJson(String scheduledMessagesJson) {
  if (scheduledMessagesJson != "") {    
    int addedScheduledMessageCount = 0;

    JsonDocument jsonDocument;
    DeserializationError deserialisationError = deserializeJson(jsonDocument, scheduledMessagesJson);

    if (!deserialisationError) {
      if (jsonDocument.is<JsonArray>()) {
        for (JsonVariant value : jsonDocument.as<JsonArray>()) {
          long scheduledDateTimeUnix = value["scheduledDateTimeUnix"];
          String message = value["message"];
          bool showIndefinitely = value["showIndefinitely"];

          addScheduledMessage(message, scheduledDateTimeUnix, showIndefinitely);
          addedScheduledMessageCount++;
        }
        
        //If there is a difference in scheduled messages, re-write the messages
        if (addedScheduledMessageCount != scheduledMessages.size()) {
          SerialPrintln("Read message count and scheduled message count differ, writing updated messages");
          writeScheduledMessagesToFile();
        }
      } 
      else {
        SerialPrintln("Invalid JSON Array found, scrapping and starting again");
        writeEmptyScheduledMessagesToFile();
      }
    }
    else {
      SerialPrintln("Invalid JSON found, scrapping and starting again");
      writeEmptyScheduledMessagesToFile();
    }
  }
}

//Convert the scheduled messages to a JSON document and save to EEPROM
void writeScheduledMessagesToFile() {
  if (scheduledMessages.size()) {
    JsonDocument scheduledMessagesDocument;
    for(int scheduledMessageIndex = 0; scheduledMessageIndex < scheduledMessages.size(); scheduledMessageIndex++) {
      ScheduledMessage scheduledMessage = scheduledMessages[scheduledMessageIndex];

      scheduledMessagesDocument[scheduledMessageIndex]["scheduledDateTimeUnix"] = scheduledMessage.ScheduledDateTimeUnix;
      scheduledMessagesDocument[scheduledMessageIndex]["message"] = scheduledMessage.Message;
      scheduledMessagesDocument[scheduledMessageIndex]["showIndefinitely"] = scheduledMessage.ShowIndefinitely;
    }

    String scheduledMessagesJson;
    serializeJson(scheduledMessagesDocument, scheduledMessagesJson);
    saveScheduledMessagesJson(scheduledMessagesJson);
  }
  else {
    SerialPrintln("No Scheduled Messages Left - Writing Empty to EEPROM");
    writeEmptyScheduledMessagesToFile();
  }
}

//Persist an empty array; used when JSON parse fails or the list is cleared
void writeEmptyScheduledMessagesToFile() {
  saveScheduledMessagesJson("[]");
}
