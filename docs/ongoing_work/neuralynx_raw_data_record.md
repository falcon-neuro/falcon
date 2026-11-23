# Neuralynx raw data A/D record

Extracted from Neuralynx's "Neuralynx Data File Formats", Revision 1.1 of 5/31/2013: the section **Raw Data A/D Record** and the section **Processing Raw Data Files** after it (pages 8 to 10), with what page 1 says of every Neuralynx file. Everything here is the document's, except the two sections marked as ours at the end.

## The file
- Raw data files end in `.nrd`. Before Cheetah 5.0.0 they ended in `.ndma`; an `.ndma` file recorded with a Digital Lynx system has the record format below, and for other acquisition systems the document refers to Neuralynx's support.
- A file starts with a 16 kilobyte ASCII text header. It only informs about the settings used for the recording, and its items may be added, changed or removed in newer versions of Cheetah.
- The data follows the header immediately, as a sequence of binary records.
- Records are not guaranteed to be complete, valid or in sequential order. Invalid records occur mostly at the beginning and the end of a recording session, where recording started while the hardware was in the middle of a record. They can usually be ignored without affecting the recorded data, but every record must be checked before its data is used.

## The record
The fields are stored in this order. Every field is 32 bits.

| Field number | Type | Field | Meaning |
|---|---|---|---|
| 0 | Int32 | STX | Start of transmission identifier. Always 2048. |
| 1 | Int32 | Packet ID | The type of packet. Always 1, the only type of the format. |
| 2 | Int32 | Packet Size | The number of A/D channels plus the number of Extra values. |
| 3 | UInt32 | Timestamp High | The high 32 bits of the 64-bit timestamp. |
| 4 | UInt32 | Timestamp Low | The low 32 bits of the 64-bit timestamp. |
| 5 | Int32 | Status | Reserved. |
| 6 | UInt32 | Parallel Input Port | The current value of the parallel input port. |
| 7 to 16 | Int32[10] | Extras | Reserved information from the hardware, currently 10 values. Their values aren't needed for analysis, but their number is needed to get the number of Data values from Packet Size. |
| 17 to 16 + N | Int32[N] | Data | One value per A/D channel of the acquisition system. N is Packet Size minus the number of Extra values. |
| 17 + N | Int32 | CRC | The CRC value for this record only. |

- A record has 18 fields besides its Data, so 18 + N fields in all.
- The timestamp is the Cheetah timestamp, an unsigned 64-bit value in microseconds.

## Checking and reading a record
The document gives five steps, to be taken in this order for every record.

1. **Find the start of a record.** Search the data in steps of one 32-bit integer for a valid STX value, 2048, before checking the rest of the record.
2. **Check the header**, the fields STX, Packet ID and Packet Size. STX must be 2048 and Packet ID must be 1. Packet Size is the size of the record that follows: the 10 Extra values plus the length of Data. If any of the three is incorrect, the record is incomplete or corrupt, and another STX must be found by step 1.
3. **Check the CRC** over all values of the record: all fields of the record, combined, must give 0. A record that doesn't is discarded. The document's example, with its spacing restored:
   ```cpp
   int recordHeaderFooterSize = 18;
   int adChannelCount = 32;
   int recordFieldCount = recordHeaderFooterSize + adChannelCount;
   int fieldIndex = 0;
   DWORD crcValue = 0;

   // loop through each field in the record
   for (fieldIndex = 0; fieldIndex < recordFieldCount; fieldIndex++) {
       // logically or all values in packet
       crcValue ^= currentPacket[fieldIndex];
   }

   // check to see if we have a valid CRC value
   if (crcValue != 0) {
       // Throw error here and discard packet
   } else {
       // Process packet data
   }
   ```
4. **Build and check the timestamp.** Timestamp High and Timestamp Low are combined into the 64-bit value. Its order is then checked: the current timestamp must be greater than the previous record's. A record out of order is discarded, and the search for an STX starts again at step 1. The document's example, with its spacing restored:
   ```cpp
   unsigned __int64 timestamp = 0;
   int timestampOffset = 3;
   unsigned int timestampLow = 0;

   // extract the timestamp high value
   timestamp = currentPacket[timestampOffset];

   // this value holds the high order bits of the timestamp, so shift it by 32 bits
   timestamp <<= 32;

   // assign timestamp low field to variable
   timestampLow = currentPacket[timestampOffset + 1];

   // load the low order bits into the current timestamp value
   timestamp += timestampLow;

   // AD records are not guaranteed to arrive in order, so the timestamp is compared
   // to the previous record's
   if (timestamp < mLastPacketTimestamp) {
       // Throw error here
   } else {
       mLastPacketTimestamp = timestamp;
   }
   ```
5. **Read the data.** The Data values are stored one after the other as 32-bit integers and need no formatting. Each value is stored by the A/D channel it came from: Data[0] holds the sample of A/D channel 0, Data[23] that of A/D channel 23. The data is stored independently of the acquisition entity setup: if two acquisition entities share an A/D channel, the record holds one value for that channel.

## Where the document's text and its examples differ (ours)
- **Step 2** calls the Extra values "the Events array".
- **Step 3**: the text says "a logical OR must be performed on all fields", and the comment in its example says the same. The example's code combines the fields with XOR (`^=`), CRC field included. Only XOR can give 0 for a record whose fields aren't all 0, so the CRC field is the XOR of the 17 + N fields before it.
- **Step 4**: the text asks for a timestamp "greater than the previous one". The example rejects only a lower one (`timestamp < mLastPacketTimestamp`), so a record with the same timestamp as the previous one passes it.
- **Step 4**: the example's comments shorten the document's here, and the document writes the variable `timestamp` once as `Timestamp`.
- The field numbers in the table are ours. They follow from the order of the fields, and agree with the examples: Timestamp High at offset 3, and 18 fields besides the Data.

## What the document doesn't say (ours)
- What a Data value means in volts: no µV per bit, no sign, and how many of the 32 bits are used.
- The sampling rate, and how far apart the timestamps of two records in a row are.
- How a sample's time becomes a timestamp in whole µs when it isn't a whole number of them, cut off or rounded. At 32 kHz a sample lasts 31.25 µs, so two timestamps in a row can only be 31 or 32 µs apart. Falcon 1 took any step up to 32 µs as normal.
- The byte order of a field. The types are named as .NET types.
- How records travel over the network. Page 1 says only that records are also sent over NetCom when streaming data to a NetCom application; the raw stream of an acquisition system isn't described.
- What the Status field and the Extra values hold, and which bit of the Parallel Input Port is which input.
