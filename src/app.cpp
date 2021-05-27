/*
  Copyright (c) 2020 Technica Engineering GmbH
  GNU General Public License v3.0+ (see LICENSE or https://www.gnu.org/licenses/gpl-3.0.txt)
*/

#include <array>
#include <codecvt>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <map>

#include <Vector/BLF.h>
#include <light_pcapng_ext.h>
#include "endianness.h"

using namespace Vector::BLF;

#define HAS_FLAG(var,pos) ((var) & (1<<(pos)))

#define NANOS_PER_SEC 1000000000
#define LINKTYPE_ETHERNET 1 
#define LINKTYPE_CAN_SOCKETCAN 227 
#define LINKTYPE_FLEXRAY 210

#define DIR_IN    1
#define DIR_OUT   2

// Enumerations
enum class FlexRayPacketType	
{
    FlexRayFrame             = 1,    // FlexRay Frame
    FlexRaySymbol            = 2     // FlexRay Symbol
};

class CanFrame {
private:
	uint8_t raw[72] = { 0 };
public:

	uint32_t id() {
		return ntoh32(*(uint32_t*)raw) & 0x1fffffff;
	}

	void id(uint32_t value) {
		uint8_t id_flags = *raw & 0xE0;
		*(uint32_t*)raw = hton32(value);
		*raw |= id_flags;
	}

	bool ext() {
		return (*raw & 0x80) != 0;
	}
	void ext(bool value) {
		uint8_t masked = *raw & 0x7F;
		*raw = masked | value << 7;
	}

	bool rtr() {
		return (*raw & 0x40) != 0;
	}
	void rtr(bool value) {
		uint8_t masked = *raw & 0xBF;
		*raw = masked | value << 6;
	}

	bool err() {
		return (*raw & 0x20) != 0;
	}
	void err(bool value) {
		uint8_t masked = *raw & 0xDF;
		*raw = masked | value << 5;
	}

	bool brs() {
		return (*(raw + 5) & 0x01) != 0;
	}
	void brs(bool value) {
		uint8_t masked = *(raw + 5) & 0xFE;
		*(raw + 5) = masked | value << 0;
	}

	bool esi() {
		return (*(raw + 5) & 0x02) != 0;
	}
	void esi(bool value) {
		uint8_t masked = *(raw + 5) & 0xFD;
		*(raw + 5) = masked | value << 1;
	}

	uint8_t len() {
		return *(raw + 4);
	}
	void len(uint8_t value) {
		*(raw + 4) = value;
	}

	const uint8_t* data() {
		return raw + 8;
	}
	void data(const uint8_t* value, size_t size) {
		memcpy(raw + 8, value, size);
	}

	const uint8_t* bytes() {
		return raw;
	}

	const uint8_t size() {
		return len() + 8;
	}

};

template <class ObjHeader>
int write_packet(
	light_pcapng pcapng,
	uint16_t link_type,
	ObjHeader* oh,
	uint32_t length,
	const uint8_t* data,
	uint64_t date_offset_ns,
	uint32_t flags = 0
) {

	light_packet_interface interface = { 0 };
	interface.link_type = link_type;
	std::string name = std::to_string(oh->channel);
	char name_str[256] = { 0 };
	memcpy(name_str, name.c_str(), sizeof(char) * std::min((size_t)255, name.length()));
	interface.name = name_str;
	
	uint64_t ts_resol = 0;
	switch (oh->objectFlags) {
	case ObjectHeader::ObjectFlags::TimeTenMics:
		ts_resol = 100000;
	case ObjectHeader::ObjectFlags::TimeOneNans:
		ts_resol = NANOS_PER_SEC;
		break;
	default:
		fprintf(stderr, "ERROR: The timestamp format is unknown (not 10us nor ns)!\n");
		return -3;
	}
	interface.timestamp_resolution = ts_resol;

	light_packet_header header = { 0 };

	uint64_t ts = (NANOS_PER_SEC / ts_resol) * oh->objectTimeStamp + date_offset_ns;

	header.timestamp.tv_sec = ts / NANOS_PER_SEC;
	header.timestamp.tv_nsec = ts % NANOS_PER_SEC;

	header.captured_length = length;
	header.original_length = length;

	return light_write_packet(pcapng, &interface, &header, data);
}

// CAN_MESSAGE = 1
void write(light_pcapng outfile, CanMessage* obj, uint64_t date_offset_ns) {
	CanFrame can;

	can.id(obj->id);
	can.rtr(HAS_FLAG(obj->flags, 7));
	can.len(obj->dlc);
	can.data(obj->data.data(), obj->data.size());

	uint32_t flags = HAS_FLAG(obj->flags, 0) ? DIR_OUT : DIR_IN;

	write_packet(outfile, LINKTYPE_CAN_SOCKETCAN, obj, can.size(), can.bytes(), date_offset_ns, flags);
}

// CAN_MESSAGE2
void write(light_pcapng outfile, CanMessage2* obj, uint64_t date_offset_ns) {
	CanFrame can;

	can.id(obj->id);
	can.rtr(HAS_FLAG(obj->flags, 7));
	can.len(obj->dlc);
	can.data(obj->data.data(), obj->data.size());

	uint32_t flags = HAS_FLAG(obj->flags, 0) ? DIR_OUT : DIR_IN;

	write_packet(outfile, LINKTYPE_CAN_SOCKETCAN, obj, can.size(), can.bytes(), date_offset_ns, flags);
}

template <class CanError>
void write_can_error(light_pcapng outfile, CanError* obj, uint64_t date_offset_ns) {

	CanFrame can;
	can.err(true);
	can.len(8);

	write_packet(outfile, LINKTYPE_CAN_SOCKETCAN, obj, can.size(), can.bytes(), date_offset_ns);
}

// CAN_ERROR = 2
void write(light_pcapng outfile, CanErrorFrame* obj, uint64_t date_offset_ns) {

	write_can_error(outfile, obj, date_offset_ns);
}

// CAN_ERROR_EXT = 73
void write(light_pcapng outfile, CanErrorFrameExt* obj, uint64_t date_offset_ns) {

	write_can_error(outfile, obj, date_offset_ns);
}

// CAN_FD_MESSAGE = 100
void write(light_pcapng outfile, CanFdMessage* obj, uint64_t date_offset_ns) {

	CanFrame can;

	can.id(obj->id);

	can.rtr(HAS_FLAG(obj->flags, 7));

	can.esi(HAS_FLAG(obj->canFdFlags, 2));
	can.brs(HAS_FLAG(obj->canFdFlags, 1));

	can.len(obj->validDataBytes);
	can.data(obj->data.data(), obj->data.size());

	uint32_t flags = HAS_FLAG(obj->flags, 0) ? DIR_OUT : DIR_IN;

	write_packet(outfile, LINKTYPE_CAN_SOCKETCAN, obj, can.size(), can.bytes(), date_offset_ns, flags);
}

// CAN_FD_MESSAGE_64 = 101
void write(light_pcapng outfile, CanFdMessage64* obj, uint64_t date_offset_ns) {

	CanFrame can;

	can.id(obj->id);

	can.rtr(HAS_FLAG(obj->flags, 4));

	can.esi(HAS_FLAG(obj->flags, 14));
	can.brs(HAS_FLAG(obj->flags, 13));

	can.len(obj->validDataBytes);
	can.data(obj->data.data(), obj->data.size());

	// TODO obj->crc

	uint32_t flags = HAS_FLAG(obj->flags, 6) || HAS_FLAG(obj->flags, 7) ? DIR_OUT : DIR_IN;

	write_packet(outfile, LINKTYPE_CAN_SOCKETCAN, obj, can.size(), can.bytes(), date_offset_ns);
}

// CAN_FD_ERROR_64 = 104
void write(light_pcapng outfile, CanFdErrorFrame64* obj, uint64_t date_offset_ns) {

	write_can_error(outfile, obj, date_offset_ns);
}

// ETHERNET_FRAME = 71
void write(light_pcapng outfile, EthernetFrame* obj, uint64_t date_offset_ns) {

	uint32_t flags = 0;
	switch (obj->dir)
	{
	case 0:
		flags = DIR_IN;
		break;
	case 1:
		flags = DIR_OUT;
		break;
	}

	std::array<uint8_t, 1518> eth;

	std::copy(obj->destinationAddress.begin(), obj->destinationAddress.end(), eth.begin());
	std::copy(obj->sourceAddress.begin(), obj->sourceAddress.end(), eth.begin() + 6);

	uint8_t header_size = 14;
	if (obj->tpid) {
		header_size += 4;
		eth[12] = (uint8_t)(obj->tpid >> 8);
		eth[13] = (uint8_t)obj->tpid;
		eth[14] = (uint8_t)(obj->tci >> 8);
		eth[15] = (uint8_t)obj->tci;
	}

	eth[header_size - 2] = (uint8_t)(obj->type >> 8);
	eth[header_size - 1] = (uint8_t)obj->type;

	std::copy(obj->payLoad.begin(), obj->payLoad.end(), eth.begin() + header_size);

	write_packet(outfile, LINKTYPE_ETHERNET, obj, obj->payLoad.size() + header_size, eth.data(), date_offset_ns, flags);
}

template <class TEthernetFrame>
void write_ethernet_frame(light_pcapng outfile, TEthernetFrame* obj, uint64_t date_offset_ns) {
	std::vector<uint8_t> eth(obj->frameData);

	if (HAS_FLAG(obj->flags, 3)) {
		uint8_t* crcPtr = (uint8_t*)&obj->frameChecksum;
		std::vector<uint8_t> crc(crcPtr, crcPtr + 4);
		eth.insert(eth.end(), crc.begin(), crc.end());
	}

	uint32_t flags = 0;
	switch (obj->dir)
	{
	case 0:
		flags = DIR_IN;
		break;
	case 1:
		flags = DIR_OUT;
		break;
	}

	write_packet(outfile, LINKTYPE_ETHERNET, obj, (uint32_t)eth.size(), eth.data(), date_offset_ns, flags);

}

// ETHERNET_FRAME_EX = 120
void write(light_pcapng outfile, EthernetFrameEx* obj, uint64_t date_offset_ns) {

	write_ethernet_frame(outfile, obj, date_offset_ns);
}

// ETHERNET_FRAME_FORWARDED = 121
void write(light_pcapng outfile, EthernetFrameForwarded* obj, uint64_t date_offset_ns) {

	write_ethernet_frame(outfile, obj, date_offset_ns);
}

void set_measurment_header(uint8_t &measurementHeader, FlexRayPacketType packetType, uint16_t channelMask = 0)
{
	/// Measurement Header (1 byte)
	// TI[0..6]: Type Index
	// 0x01: FlexRay Frame
	// 0x02: FlexRay Symbol
	switch (packetType)
	{
	case FlexRayPacketType::FlexRayFrame:
		measurementHeader = 0x01;
		break;
	case FlexRayPacketType::FlexRaySymbol:
		measurementHeader = 0x02;
		break;
	}
	// CH: Channel, indicates the Channel
	// 1	: Channel A
	// 2/3	: Channel B
	switch (channelMask)
	{
	case 1: /* Channel A */
		break;
	case 2: /* Channel B */
	case 3: /* Channel B */
		measurementHeader |= 0x80;
		break;
	}
}

void set_header_crc(uint16_t channelMask, uint16_t headerCrc1, uint16_t headerCrc2, uint16_t &headerCrc)
{
	// CH: Channel, indicates the Channel
	// 1	: Channel A
	// 2/3	: Channel B
	switch (channelMask)
	{
	case 1: /* Channel A */
		headerCrc = headerCrc1;
		break;
	case 2: /* Channel B */
	case 3: /* Channel B */
		headerCrc = headerCrc2;
		break;
	}
}

void set_header_flags(uint16_t frameState, uint8_t &headerFlags)
{
	if (HAS_FLAG(frameState, 0))
	{
		headerFlags |= 0x08; // Payload preample indicator bit set to 1
	}
	if (HAS_FLAG(frameState, 1))
	{
		headerFlags |= 0x02; // Sync. frame indicator bit set to 1
	}
	if (HAS_FLAG(frameState, 2))
	{
		headerFlags |= 0x10; // Reserved bit set to 1
	}
	if (!HAS_FLAG(frameState, 3))
	{
		headerFlags |= 0x04; // Null frame indicator bit set to 1
	}
	if (HAS_FLAG(frameState, 4))
	{
		headerFlags |= 0x01; // Startup frame indicator bit set to 1
	}
}

void set_header_flags_rcv_msg(uint32_t frameFlags, uint8_t &headerFlags)
{
	if (!HAS_FLAG(frameFlags, 0))
	{
		headerFlags |= 0x04; // Null frame indicator bit set to 1
	}
	if (HAS_FLAG(frameFlags, 2))
	{
		headerFlags |= 0x02; // Sync. frame indicator bit set to 1
	}
	if (HAS_FLAG(frameFlags, 3))
	{
		headerFlags |= 0x01; // Startup frame indicator bit set to 1
	}
	if (HAS_FLAG(frameFlags, 4))
	{
		headerFlags |= 0x08; // Payload preample indicator bit set to 1
	}
	if (HAS_FLAG(frameFlags, 5))
	{
		headerFlags |= 0x10; // Reserved bit set to 1
	}
}

void set_header(uint64_t &header, uint8_t headerFlags, uint64_t payloadLength, uint8_t cycleCount = 0, uint16_t frameId = 0, uint16_t headerCrc = 0)
{
	header = (static_cast<uint64_t>(headerFlags) << 35) | (static_cast<uint64_t>(payloadLength & 0x7F) << 17);
	if (cycleCount != 0)
	{
		header |= static_cast<uint64_t>(cycleCount & 0x3F);
	}
	if (frameId != 0)
	{
		header |= (static_cast<uint64_t>(frameId & 0x07FF) << 24);
	}
	if (headerCrc != 0)
	{
		header |= (static_cast<uint64_t>(headerCrc & 0x07FF) << 6);
	}

	// Convert from Host Byte Order to Network Byte Order (network order is big endian)
	header = hton64(header);
}

// FLEXRAY_DATA = 29
void write(light_pcapng outfile, FlexRayData* obj, uint64_t date_offset_ns) {

	uint64_t header = 0;
	uint8_t headerFlags = 0;
	std::array<uint8_t, 261> flexrayData;

	memset(&flexrayData, 0, sizeof(flexrayData));

	/// Measurement Header (1 byte)
	set_measurment_header(flexrayData[0], FlexRayPacketType::FlexRayFrame);

	/// Error Flags Information (1 byte) -> set to 0

	/// FlexRay Frame Header (5 bytes)
	//  - Header flags
	headerFlags |= 0x04; // Null Frame: False (indicator bit set to 1)
	//  - Payload length
	uint64_t len = obj->dataBytes.size() / 2;
	set_header(header, headerFlags, len, 0, obj->messageId, obj->crc);

	// Copy only 5 bytes of header to flexrayData
	uint8_t * headerPtr = (uint8_t *)&header;
	memcpy(&flexrayData[2], headerPtr + 3, 5);

	// FlexRay Frame Payload (0-254 bytes)
	std::copy(obj->dataBytes.begin(), obj->dataBytes.end(), flexrayData.begin() + 7);

	write_packet(outfile, LINKTYPE_FLEXRAY, obj, obj->dataBytes.size() + 7, flexrayData.data(), date_offset_ns);
}

// FLEXRAY_SYNC = 30
void write(light_pcapng outfile, FlexRaySync* obj, uint64_t date_offset_ns) {

	uint64_t header = 0;
	uint8_t headerFlags = 0;
	std::array<uint8_t, 261> flexrayData;

	memset(&flexrayData, 0, sizeof(flexrayData));

	/// Measurement Header (1 byte)
	set_measurment_header(flexrayData[0], FlexRayPacketType::FlexRayFrame);

	/// Error Flags Information (1 byte) -> set to 0

	/// FlexRay Frame Header (5 bytes)
	//  - Header flags
	headerFlags |= 0x04; // Null Frame: False (indicator bit set to 1)
	headerFlags |= 0x02; // Sync. frame indicator bit set to 1

	/// FlexRay Frame Header (5 bytes)
	//  - Payload length
	uint64_t len = obj->dataBytes.size() / 2;
	set_header(header, headerFlags, len, obj->cycle, obj->messageId, obj->crc);

	// Copy only 5 bytes of header to flexrayData
	uint8_t * headerPtr = (uint8_t *)&header;
	memcpy(&flexrayData[2], headerPtr + 3, 5);

	// FlexRay Frame Payload (0-254 bytes)
	std::copy(obj->dataBytes.begin(), obj->dataBytes.end(), flexrayData.begin() + 7);

	write_packet(outfile, LINKTYPE_FLEXRAY, obj, obj->dataBytes.size() + 7, flexrayData.data(), date_offset_ns);
}

// FLEXRAY_CYCLE = 40
void write(light_pcapng outfile, FlexRayV6StartCycleEvent* obj, uint64_t date_offset_ns) {

	uint64_t header = 0;
	uint8_t headerFlags = 0;
	std::array<uint8_t, 261> flexrayData;

	memset(&flexrayData, 0, sizeof(flexrayData));

	/// Measurement Header (1 byte)
	set_measurment_header(flexrayData[0], FlexRayPacketType::FlexRayFrame);

	/// Error Flags Information (1 byte) -> set to 0

	/// FlexRay Frame Header (5 bytes)
	//  - Header flags
	headerFlags |= 0x04; // Null Frame: False (indicator bit set to 1)
	//  - Payload length
	uint64_t len = obj->dataBytes.size() / 2;
	set_header(header, headerFlags, len);

	// Copy only 5 bytes of header to flexrayData
	uint8_t * headerPtr = (uint8_t *)&header;
	memcpy(&flexrayData[2], headerPtr + 3, 5);

	// FlexRay Frame Payload (0-254 bytes)
	std::copy(obj->dataBytes.begin(), obj->dataBytes.end(), flexrayData.begin() + 7);

	write_packet(outfile, LINKTYPE_FLEXRAY, obj, obj->dataBytes.size() + 7, flexrayData.data(), date_offset_ns);
}

// FLEXRAY_MESSAGE = 41
void write(light_pcapng outfile, FlexRayV6Message* obj, uint64_t date_offset_ns) {

	uint64_t header = 0;
	uint8_t headerFlags = 0;
	std::array<uint8_t, 261> flexrayData;

	memset(&flexrayData, 0, sizeof(flexrayData));

	/// Measurement Header (1 byte)
	set_measurment_header(flexrayData[0], FlexRayPacketType::FlexRayFrame);

	/// Error Flags Information (1 byte) -> set to 0

	/// FlexRay Frame Header (5 bytes)
	set_header_flags(obj->frameState, headerFlags);
	uint64_t len = obj->dataBytes.size() / 2;
	set_header(header, headerFlags, len, obj->cycle, obj->frameId, obj->headerCrc);

	// Copy only 5 bytes of header to flexrayData
	uint8_t * headerPtr = (uint8_t *)&header;
	memcpy(&flexrayData[2], headerPtr + 3, 5);

	// FlexRay Frame Payload (0-254 bytes)
	std::copy(obj->dataBytes.begin(), obj->dataBytes.end(), flexrayData.begin() + 7);

	write_packet(outfile, LINKTYPE_FLEXRAY, obj, obj->dataBytes.size() + 7, flexrayData.data(), date_offset_ns);
}

// FR_ERROR = 47
void write(light_pcapng outfile, FlexRayVFrError* obj, uint64_t date_offset_ns) {

	uint64_t header = 0;
	uint8_t headerFlags = 0;
	std::array<uint8_t, 7> flexrayData;

	memset(&flexrayData, 0, sizeof(flexrayData));

	/// Measurement Header (1 byte)
	set_measurment_header(flexrayData[0], FlexRayPacketType::FlexRayFrame, obj->channelMask);

	/// Error Flags Information (1 byte)
	flexrayData[1] |= 0x02; // Coding error bit (CODERR) set to 1

	/// FlexRay Frame Header (5 bytes)
	//  - Header flags
	headerFlags |= 0x04; // Null Frame: False (indicator bit set to 1)
	set_header(header, headerFlags, 0, obj->cycle);

	// Copy only 5 bytes of header to flexrayData
	uint8_t * headerPtr = (uint8_t *)&header;
	memcpy(&flexrayData[2], headerPtr + 3, 5);

	/// FlexRay Frame Payload (0-254 bytes) -> no payload

	write_packet(outfile, LINKTYPE_FLEXRAY, obj, 7, flexrayData.data(), date_offset_ns);
}

// FR_STATUS = 48
void write(light_pcapng outfile, FlexRayVFrStatus* obj, uint64_t date_offset_ns) {

	std::array<uint8_t, 2> flexraySymbolData;

	memset(&flexraySymbolData, 0, sizeof(flexraySymbolData));

	/// Measurement Header (1 byte)
	set_measurment_header(flexraySymbolData[0], FlexRayPacketType::FlexRaySymbol, obj->channelMask);

	/// Symbol length (1 byte)
	if (obj->tag == 3) /* BUSDOCTOR */
	{
		flexraySymbolData[1] = obj->data[1] & 0xFF;
	}
	if (obj->tag == 5) /* VN-Interface */
	{
		flexraySymbolData[1] = obj->data[0] & 0xFF;
	}

	write_packet(outfile, LINKTYPE_FLEXRAY, obj, 2, flexraySymbolData.data(), date_offset_ns);
}

// FR_STARTCYCLE = 49
void write(light_pcapng outfile, FlexRayVFrStartCycle* obj, uint64_t date_offset_ns) {

	uint64_t header = 0;
	uint8_t headerFlags = 0;
	std::array<uint8_t, 19> flexrayData;

	memset(&flexrayData, 0, sizeof(flexrayData));

	/// Measurement Header (1 byte)
	set_measurment_header(flexrayData[0], FlexRayPacketType::FlexRayFrame, obj->channelMask);

	/// Error Flags Information (1 byte) -> set to 0

	/// FlexRay Frame Header (5 bytes)
	//  - Header flags
	headerFlags |= 0x04; // Null Frame: False (indicator bit set to 1)
	//  - Payload length
	uint64_t len = obj->dataBytes.size() / 2;
	set_header(header, headerFlags, len, obj->cycle);

	// Copy only 5 bytes of header to flexrayData
	uint8_t * headerPtr = (uint8_t *)&header;
	memcpy(&flexrayData[2], headerPtr + 3, 5);

	// FlexRay Frame Payload (0-254 bytes)
	std::copy(obj->dataBytes.begin(), obj->dataBytes.end(), flexrayData.begin() + 7);

	write_packet(outfile, LINKTYPE_FLEXRAY, obj, obj->dataBytes.size() + 7, flexrayData.data(), date_offset_ns);
}

// FR_RCVMESSAGE = 50
void write(light_pcapng outfile, FlexRayVFrReceiveMsg* obj, uint64_t date_offset_ns) {

	uint64_t header = 0;
	uint16_t headerCrc = 0;
	uint8_t headerFlags = 0;
	std::array<uint8_t, 261> flexrayData;

	memset(&flexrayData, 0, sizeof(flexrayData));

	/// Measurement Header (1 byte)
	set_measurment_header(flexrayData[0], FlexRayPacketType::FlexRayFrame, obj->channelMask);

	/// Error Flags Information (1 byte) -> case Error flag (error frame or invalid frame) set to 1
	if (HAS_FLAG(obj->frameFlags, 6))
	{
		flexrayData[1] |= 0x10; // FCRCERR bit set to 1
	}

	/// FlexRay Frame Header (5 bytes)
	//  - Header flags
	set_header_flags_rcv_msg(obj->frameFlags, headerFlags);
	// 	- Header CRC
	set_header_crc(obj->channelMask, obj->headerCrc1, obj->headerCrc2, headerCrc);
	//  - Payload length
	uint64_t len = obj->dataBytes.size() / 2;
	set_header(header, headerFlags, len, obj->cycle, obj->frameId, headerCrc);

	// Copy only 5 bytes of header to flexrayData
	uint8_t * headerPtr = (uint8_t *)&header;
	memcpy(&flexrayData[2], headerPtr + 3, 5);

	// FlexRay Frame Payload (0-254 bytes)
	std::copy(obj->dataBytes.begin(), obj->dataBytes.end(), flexrayData.begin() + 7);

	write_packet(outfile, LINKTYPE_FLEXRAY, obj, obj->dataBytes.size() + 7, flexrayData.data(), date_offset_ns);
}

// FR_RCVMESSAGE_EX = 66
void write(light_pcapng outfile, FlexRayVFrReceiveMsgEx* obj, uint64_t date_offset_ns) {

	uint64_t header = 0;
	uint16_t headerCrc = 0;
	uint8_t headerFlags = 0;
	uint8_t measurementHeader = 0;
	uint8_t errorFlagsInfo = 0;
	std::vector<uint8_t> flexrayData;

	flexrayData.clear();

	/// Measurement Header (1 byte)
	set_measurment_header(measurementHeader, FlexRayPacketType::FlexRayFrame, obj->channelMask);

	flexrayData.push_back(measurementHeader);

	/// Error Flags Information (1 byte) -> case Error flag (error frame or invalid frame) set to 1
	if (HAS_FLAG(obj->frameFlags, 6))
	{
		errorFlagsInfo |= 0x10; // FCRCERR bit set to 1
	}
	flexrayData.push_back(errorFlagsInfo);

	/// FlexRay Frame Header (5 bytes)
	//  - Header flags
	set_header_flags_rcv_msg(obj->frameFlags, headerFlags);
	// 	- Header CRC
	set_header_crc(obj->channelMask, obj->headerCrc1, obj->headerCrc2, headerCrc);
	//  - Payload length
	uint64_t len = obj->dataBytes.size() / 2;
	set_header(header, headerFlags, len, obj->cycle, obj->frameId, headerCrc);

	// Copy only 5 bytes of header to flexrayData
	uint8_t * headerPtr = (uint8_t *)&header;
	std::vector<uint8_t> headerVec(headerPtr + 3, headerPtr + 8);
	flexrayData.insert(flexrayData.end(), headerVec.begin(), headerVec.end());

	// FlexRay Frame Payload (0-254 bytes)
	flexrayData.insert(flexrayData.end(), obj->dataBytes.begin(), obj->dataBytes.end());

	write_packet(outfile, LINKTYPE_FLEXRAY, obj, obj->dataBytes.size() + 7, flexrayData.data(), date_offset_ns);
}

uint64_t calculate_startdate(Vector::BLF::File* infile) {
	Vector::BLF::SYSTEMTIME startTime;
	startTime = infile->fileStatistics.measurementStartTime;

	struct tm tms = { 0 };
	tms.tm_year = startTime.year - 1900;
	tms.tm_mon = startTime.month - 1;
	tms.tm_mday = startTime.day;
	tms.tm_hour = startTime.hour;
	tms.tm_min = startTime.minute;
	tms.tm_sec = startTime.second;

	time_t ret = mktime(&tms);

	ret *= 1000;
	ret += startTime.milliseconds;
	ret *= 1000 * 1000;

	return ret;
}

int main(int argc, char* argv[]) {
	if (argc != 3) {
		fprintf(stderr, "Usage %s [infile] [outfile]\n", argv[0]);
		return 1;
	}

	Vector::BLF::File infile;
	infile.open(argv[1]);
	if (!infile.is_open()) {
		fprintf(stderr, "Unable to open: %s\n", argv[1]);
		return 1;
	}
	light_pcapng outfile = light_pcapng_open(argv[2], "wb");
	if (!outfile) {
		fprintf(stderr, "Unable to open: %s\n", argv[2]);
		return 1;
	}

	uint64_t startDate_ns = calculate_startdate(&infile);

	while (infile.good()) {
		ObjectHeaderBase* ohb = nullptr;

		/* read and capture exceptions, e.g. unfinished files */
		try {
			ohb = infile.read();
		}
		catch (std::runtime_error& e) {
			std::cout << "Exception: " << e.what() << std::endl;
		}
		if (ohb == nullptr) {
			break;
		}
		/* Object */
		switch (ohb->objectType) {

		case ObjectType::CAN_MESSAGE:
			write(outfile, reinterpret_cast<CanMessage*>(ohb), startDate_ns);
			break;

		case ObjectType::CAN_ERROR:
			write(outfile, reinterpret_cast<CanErrorFrame*>(ohb), startDate_ns);
			break;

		case ObjectType::CAN_FD_MESSAGE:
			write(outfile, reinterpret_cast<CanFdMessage*>(ohb), startDate_ns);
			break;

		case ObjectType::CAN_FD_MESSAGE_64:
			write(outfile, reinterpret_cast<CanFdMessage64*>(ohb), startDate_ns);
			break;

		case ObjectType::CAN_FD_ERROR_64:
			write(outfile, reinterpret_cast<CanFdErrorFrame64*>(ohb), startDate_ns);
			break;

		case ObjectType::ETHERNET_FRAME:
			write(outfile, reinterpret_cast<EthernetFrame*>(ohb), startDate_ns);
			break;

		case ObjectType::CAN_ERROR_EXT:
			write(outfile, reinterpret_cast<CanErrorFrameExt*>(ohb), startDate_ns);
			break;

		case ObjectType::CAN_MESSAGE2:
			write(outfile, reinterpret_cast<CanMessage2*>(ohb), startDate_ns);
			break;

		case ObjectType::ETHERNET_FRAME_EX:
			write(outfile, reinterpret_cast<EthernetFrameEx*>(ohb), startDate_ns);
			break;

		case ObjectType::ETHERNET_FRAME_FORWARDED:
			write(outfile, reinterpret_cast<EthernetFrameForwarded*>(ohb), startDate_ns);
			break;

		case ObjectType::FLEXRAY_DATA:
			write(outfile, reinterpret_cast<FlexRayData*>(ohb), startDate_ns);
			break;

		case ObjectType::FLEXRAY_SYNC:
			write(outfile, reinterpret_cast<FlexRaySync*>(ohb), startDate_ns);
			break;

		case ObjectType::FLEXRAY_CYCLE:
			write(outfile, reinterpret_cast<FlexRayV6StartCycleEvent*>(ohb), startDate_ns);
			break;

		case ObjectType::FLEXRAY_MESSAGE:
			write(outfile, reinterpret_cast<FlexRayV6Message*>(ohb), startDate_ns);
			break;

		case ObjectType::FLEXRAY_STATUS:
			// We do not have reliable BLF file or clear documentation for this type
			break;

		case ObjectType::FR_ERROR:
			write(outfile, reinterpret_cast<FlexRayVFrError*>(ohb), startDate_ns);
			break;

		case ObjectType::FR_STATUS:
			write(outfile, reinterpret_cast<FlexRayVFrStatus*>(ohb), startDate_ns);
			break;

		case ObjectType::FR_STARTCYCLE:
			write(outfile, reinterpret_cast<FlexRayVFrStartCycle*>(ohb), startDate_ns);
			break;

		case ObjectType::FR_RCVMESSAGE:
			write(outfile, reinterpret_cast<FlexRayVFrReceiveMsg*>(ohb), startDate_ns);
			break;

		case ObjectType::FR_RCVMESSAGE_EX:
			write(outfile, reinterpret_cast<FlexRayVFrReceiveMsgEx*>(ohb), startDate_ns);
			break;

		}

		/* delete object */
		delete ohb;
	}

	infile.close();
	light_pcapng_close(outfile);

	return 0;
}
