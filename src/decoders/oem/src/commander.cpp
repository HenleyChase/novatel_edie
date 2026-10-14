// ===============================================================================
// |                                                                             |
// |  COPYRIGHT NovAtel Inc, 2022. All rights reserved.                          |
// |                                                                             |
// |  Permission is hereby granted, free of charge, to any person obtaining a    |
// |  copy of this software and associated documentation files (the "Software"), |
// |  to deal in the Software without restriction, including without limitation  |
// |  the rights to use, copy, modify, merge, publish, distribute, sublicense,   |
// |  and/or sell copies of the Software, and to permit persons to whom the      |
// |  Software is furnished to do so, subject to the following conditions:       |
// |                                                                             |
// |  The above copyright notice and this permission notice shall be included    |
// |  in all copies or substantial portions of the Software.                     |
// |                                                                             |
// |  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR |
// |  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   |
// |  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    |
// |  THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER |
// |  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING    |
// |  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER        |
// |  DEALINGS IN THE SOFTWARE.                                                  |
// |                                                                             |
// ===============================================================================
// ! \file commander.cpp
// ===============================================================================

#include "novatel_edie/decoders/oem/commander.hpp"

#include <iomanip>
#include <limits>
#include <sstream>

using namespace novatel::edie;
using namespace novatel::edie::oem;

namespace {

// -------------------------------------------------------------------------------------------------------
// Split an abbreviated ASCII command into its parameters. Parameters are separated by spaces, except within quotes.
std::vector<std::string> SplitAbbrevAsciiParams(const std::string& strParams_)
{
    std::vector<std::string> vParams;
    bool bInParam = false;
    bool bInQuotes = false;

    for (const char c : strParams_)
    {
        if (c == ' ' && !bInQuotes)
        {
            bInParam = false;
            continue;
        }

        if (!bInParam) { vParams.emplace_back(); }
        bInParam = true;

        vParams.back() += c;
        if (c == '"') { bInQuotes = !bInQuotes; }
    }

    return vParams;
}

// -------------------------------------------------------------------------------------------------------
// Join parameters back into an abbreviated ASCII command.
std::string JoinAbbrevAsciiParams(const std::vector<std::string>& vParams_)
{
    std::string strParams;

    for (const std::string& strParam : vParams_)
    {
        if (!strParams.empty()) { strParams += ' '; }
        strParams += strParam;
    }

    return strParams;
}

// -------------------------------------------------------------------------------------------------------
// The number of parameters a field takes in an abbreviated ASCII command, or 0 if it can vary.
size_t FieldParamCount(const BaseField& clField_)
{
    switch (clField_.type)
    {
    case FIELD_TYPE::SIMPLE: [[fallthrough]];
    case FIELD_TYPE::ENUM: [[fallthrough]];
    case FIELD_TYPE::STRING: return 1;
    case FIELD_TYPE::FIXED_LENGTH_ARRAY:
        // Arrays of numbers take one parameter per element, other arrays (e.g. strings) are a single parameter
        if (clField_.isCsv) { return dynamic_cast<const ArrayField&>(clField_).arrayLength; }
        return 1;
    default: return 0;
    }
}

// -------------------------------------------------------------------------------------------------------
// The number of parameters all the fields take in an abbreviated ASCII command, or 0 if it can vary.
size_t FieldsParamCount(const std::vector<BaseField::ConstPtr>& vFields_)
{
    size_t uiParamCount = 0;

    for (const auto& pclField : vFields_)
    {
        const size_t uiFieldParamCount = FieldParamCount(*pclField);
        if (uiFieldParamCount == 0) { return 0; }
        uiParamCount += uiFieldParamCount;
    }

    return uiParamCount;
}

// -------------------------------------------------------------------------------------------------------
// Check if a parameter is one of the values of an enum field.
bool IsEnumParam(const BaseField& clField_, const std::string& strParam_)
{
    const EnumDefinition::ConstPtr& pclEnumDef = dynamic_cast<const EnumField&>(clField_).enumDef;
    return pclEnumDef != nullptr && pclEnumDef->nameValue.count(strParam_) != 0;
}

// -------------------------------------------------------------------------------------------------------
// Check if a field is a port, which is an enum with a THISPORT value.
bool IsPortField(const BaseField& clField_) { return clField_.type == FIELD_TYPE::ENUM && IsEnumParam(clField_, "THISPORT"); }

// -------------------------------------------------------------------------------------------------------
// Get the database default of a field as an abbreviated ASCII parameter. Returns false if it has none.
bool GetDefaultParam(const BaseField& clField_, std::string& strParam_)
{
    if (!clField_.defaultValue.has_value()) { return false; }

    // A message name, such as the log in "LOG", has no meaningful default
    if (clField_.conversionHash == CalculateBlockCrc32("m")) { return false; }

    const double dDefault = clField_.defaultValue.value();

    if (clField_.type == FIELD_TYPE::ENUM)
    {
        const EnumDefinition::ConstPtr& pclEnumDef = dynamic_cast<const EnumField&>(clField_).enumDef;
        if (pclEnumDef == nullptr) { return false; }

        const auto itEnumerator = pclEnumDef->valueName.find(static_cast<uint32_t>(dDefault));
        if (itEnumerator == pclEnumDef->valueName.end()) { return false; }

        strParam_ = itEnumerator->second;
        return true;
    }

    if (clField_.type != FIELD_TYPE::SIMPLE) { return false; }

    std::ostringstream oss;
    if (clField_.dataType.name == DATA_TYPE::BOOL) { oss << (dDefault == 0.0 ? "FALSE" : "TRUE"); }
    else if (clField_.conversionHash == CalculateBlockCrc32("x") || clField_.conversionHash == CalculateBlockCrc32("X") ||
             clField_.conversionHash == CalculateBlockCrc32("XB"))
    {
        oss << std::hex << static_cast<uint64_t>(dDefault);
    }
    else { oss << std::setprecision(std::numeric_limits<double>::max_digits10) << dDefault; }

    strParam_ = oss.str();
    return true;
}

// -------------------------------------------------------------------------------------------------------
// The receiver accepts abbreviated ASCII commands with parameters left off: a port at the start, such as in
// "LOG BESTPOSA ONCE", and any at the end, such as everything after the message in "LOG VERSIONA". The decoder needs a
// value for every field, so fill in the database defaults of the fields that were left off.
STATUS AddDefaultParams(const FieldInfo& clFieldInfo_, std::string& strParams_)
{
    const std::vector<BaseField::ConstPtr>& vFields = clFieldInfo_.messageOrderedFields;
    if (vFields.empty()) { return STATUS::SUCCESS; }

    std::vector<std::string> vParams = SplitAbbrevAsciiParams(strParams_.substr(0, strParams_.find_first_of("\r\n")));
    const size_t uiFieldsParamCount = FieldsParamCount(vFields);
    std::string strDefault;

    // The port was left off if a parameter is missing and the first one isn't a port
    if (!vParams.empty() && (uiFieldsParamCount == 0 || vParams.size() < uiFieldsParamCount) && IsPortField(*vFields.front()) &&
        !IsEnumParam(*vFields.front(), vParams.front()) && GetDefaultParam(*vFields.front(), strDefault))
    {
        vParams.insert(vParams.begin(), strDefault);
    }

    // Check the parameters that were given against their fields
    size_t uiParam = 0;
    size_t uiField = 0;

    for (; uiField < vFields.size() && uiParam < vParams.size(); uiField++)
    {
        const size_t uiFieldParamCount = FieldParamCount(*vFields[uiField]);

        // Leave anything that can't be matched to the fields for the decoder to reject
        if (uiFieldParamCount == 0 || uiFieldParamCount > vParams.size() - uiParam)
        {
            strParams_ = JoinAbbrevAsciiParams(vParams);
            return STATUS::SUCCESS;
        }

        // The decoder would take an unknown enum value as UNKNOWN, which is never a valid command
        if (vFields[uiField]->type == FIELD_TYPE::ENUM && !IsEnumParam(*vFields[uiField], vParams[uiParam])) { return STATUS::MALFORMED_INPUT; }

        uiParam += uiFieldParamCount;
    }

    // The decoder would ignore extra parameters, which are likely a typo
    if (uiParam < vParams.size()) { return STATUS::MALFORMED_INPUT; }

    // Add the defaults of the fields that were left off the end
    for (; uiField < vFields.size() && GetDefaultParam(*vFields[uiField], strDefault); uiField++) { vParams.push_back(strDefault); }

    strParams_ = JoinAbbrevAsciiParams(vParams);
    return STATUS::SUCCESS;
}

} // namespace

// -------------------------------------------------------------------------------------------------------
Commander::Commander(MessageDatabase::Ptr pclMessageDb_) : clMyMessageDecoder(pclMessageDb_), clMyEncoder(pclMessageDb_)
{
    pclMyLogger->debug("Commander initializing...");
    if (pclMessageDb_ != nullptr) { LoadJsonDb(pclMessageDb_); }
    pclMyLogger->debug("Commander initialized");
}

// -------------------------------------------------------------------------------------------------------
void Commander::LoadJsonDb(MessageDatabase::Ptr pclMessageDb_)
{
    pclMyMsgDb = pclMessageDb_;
    InitEnumDefinitions();
}

// -------------------------------------------------------------------------------------------------------
void Commander::InitEnumDefinitions()
{
    vMyCommandDefinitions = pclMyMsgDb->GetEnumDefName("Commands");
    vMyPortAddressDefinitions = pclMyMsgDb->GetEnumDefName("PortAddress");
    vMyGpsTimeStatusDefinitions = pclMyMsgDb->GetEnumDefName("GPSTimeStatus");
}

// -------------------------------------------------------------------------------------------------------
STATUS Commander::Encode(const char* pcAbbrevAsciiCommand_, const uint32_t uiAbbrevAsciiCommandLength_, char* pcEncodeBuffer_,
                         uint32_t& uiEncodeBufferSize_, const ENCODE_FORMAT eEncodeFormat_)
{
    constexpr uint32_t thisPort = 0xC0;

    if ((pcAbbrevAsciiCommand_ == nullptr) || (pcEncodeBuffer_ == nullptr)) { return STATUS::NULL_PROVIDED; }

    if (eEncodeFormat_ != ENCODE_FORMAT::ASCII && eEncodeFormat_ != ENCODE_FORMAT::BINARY) { return STATUS::UNSUPPORTED; }

    const auto strAbbrevAsciiCommand = std::string(pcAbbrevAsciiCommand_, uiAbbrevAsciiCommandLength_);
    const size_t ullPos = strAbbrevAsciiCommand.find_first_of(' ');
    const std::string strCmdName = strAbbrevAsciiCommand.substr(0, ullPos);
    std::string strCmdParams;
    if (ullPos != std::string::npos) { strCmdParams = strAbbrevAsciiCommand.substr(ullPos + 1); }

    if (pclMyMsgDb == nullptr) { return STATUS::NO_DATABASE; }

    MessageDefinition::ConstPtr pclMessageDef = pclMyMsgDb->GetMsgDef(strCmdName);
    if (!pclMessageDef) { return STATUS::NO_DEFINITION; }

    const STATUS eParamsStatus = AddDefaultParams(*pclMessageDef->fieldInfo.begin()->second, strCmdParams);
    if (eParamsStatus != STATUS::SUCCESS) { return eParamsStatus; }
    if (strCmdParams.size() >= MAX_ASCII_MESSAGE_LENGTH) { return STATUS::BUFFER_FULL; }

    char acCmdParams[MAX_ASCII_MESSAGE_LENGTH];
    char* pcCmdParams = acCmdParams;
    std::copy(strCmdParams.begin(), strCmdParams.end(), acCmdParams);
    acCmdParams[strCmdParams.size()] = '\0';

    MessageDataStruct stMessageData;
    MetaDataStruct stMetaData;
    IntermediateHeader stIntermediateHeader;
    CompositeField stIntermediateMessage;

    // Prime the metadata with information we already know
    stMetaData.eFormat = DECODE_FORMAT::ABB_ASCII;
    stMetaData.usMessageId = static_cast<uint16_t>(pclMessageDef->logID);
    stMetaData.uiMessageCrc = static_cast<uint32_t>(pclMessageDef->fieldInfo.begin()->first);

    STATUS eStatus = clMyMessageDecoder.Decode(reinterpret_cast<unsigned char*>(pcCmdParams), stIntermediateMessage, stMetaData);
    if (eStatus != STATUS::SUCCESS) { return eStatus; }

    // Prime the intermediate header with information we already know
    stIntermediateHeader.uiPortAddress = thisPort;
    stIntermediateHeader.usMessageId = stMetaData.usMessageId;
    stIntermediateHeader.uiMessageDefinitionCrc = stMetaData.uiMessageCrc;

    auto* pucEncodeBuffer = reinterpret_cast<unsigned char*>(pcEncodeBuffer_);
    eStatus = clMyEncoder.Encode(&pucEncodeBuffer, uiEncodeBufferSize_, stIntermediateHeader, stIntermediateMessage, stMessageData, eEncodeFormat_);

    if (eStatus != STATUS::SUCCESS) { return eStatus; }

    // Null-terminate the command, if possible. Otherwise, the command will be the size of the buffer.
    if (stMessageData.uiMessageLength < uiEncodeBufferSize_) { stMessageData.pucMessage[stMessageData.uiMessageLength] = '\0'; }
    uiEncodeBufferSize_ = stMessageData.uiMessageLength;

    return STATUS::SUCCESS;
}

// -------------------------------------------------------------------------------------------------------
STATUS Commander::Encode(const MessageDatabase& clJsonDb_, const MessageDecoder& clMessageDecoder_, Encoder& clEncoder_,
                         const char* pcAbbrevAsciiCommand_, const uint32_t uiAbbrevAsciiCommandLength_, char* pcEncodeBuffer_,
                         uint32_t& uiEncodeBufferSize_, const ENCODE_FORMAT eEncodeFormat_)
{
    constexpr uint32_t thisPort = 0xC0;

    if ((pcAbbrevAsciiCommand_ == nullptr) || (pcEncodeBuffer_ == nullptr)) { return STATUS::NULL_PROVIDED; }

    if (eEncodeFormat_ != ENCODE_FORMAT::ASCII && eEncodeFormat_ != ENCODE_FORMAT::BINARY) { return STATUS::UNSUPPORTED; }

    const auto strAbbrevAsciiCommand = std::string(pcAbbrevAsciiCommand_, uiAbbrevAsciiCommandLength_);
    const size_t ullPos = strAbbrevAsciiCommand.find_first_of(' ');
    const std::string strCmdName = strAbbrevAsciiCommand.substr(0, ullPos);
    std::string strCmdParams;
    if (ullPos != std::string::npos) { strCmdParams = strAbbrevAsciiCommand.substr(ullPos + 1); }

    MessageDefinition::ConstPtr pclMessageDef = clJsonDb_.GetMsgDef(strCmdName);
    if (!pclMessageDef) { return STATUS::NO_DEFINITION; }

    const STATUS eParamsStatus = AddDefaultParams(*pclMessageDef->fieldInfo.begin()->second, strCmdParams);
    if (eParamsStatus != STATUS::SUCCESS) { return eParamsStatus; }
    if (strCmdParams.size() >= MAX_ASCII_MESSAGE_LENGTH) { return STATUS::BUFFER_FULL; }

    unsigned char acCmdParams[MAX_ASCII_MESSAGE_LENGTH];
    unsigned char* pucCmdParams = acCmdParams;
    std::copy(strCmdParams.begin(), strCmdParams.end(), acCmdParams);
    acCmdParams[strCmdParams.size()] = '\0';

    MessageDataStruct stMessageData;
    MetaDataStruct stMetaData;
    IntermediateHeader stIntermediateHeader;
    CompositeField stIntermediateMessage;

    // Prime the metadata with information we already know
    stMetaData.eFormat = DECODE_FORMAT::ABB_ASCII;
    stMetaData.usMessageId = static_cast<uint16_t>(pclMessageDef->logID);
    stMetaData.uiMessageCrc = static_cast<uint32_t>(pclMessageDef->fieldInfo.begin()->first);

    const STATUS eDecoderStatus = clMessageDecoder_.Decode(pucCmdParams, stIntermediateMessage, stMetaData);
    if (eDecoderStatus != STATUS::SUCCESS) { return eDecoderStatus; }

    // Prime the intermediate header with information we already know
    stIntermediateHeader.uiPortAddress = thisPort;
    stIntermediateHeader.usMessageId = stMetaData.usMessageId;
    stIntermediateHeader.uiMessageDefinitionCrc = stMetaData.uiMessageCrc;

    auto* pucEncodeBuffer = reinterpret_cast<unsigned char*>(pcEncodeBuffer_);
    const STATUS eEncoderStatus =
        clEncoder_.Encode(&pucEncodeBuffer, uiEncodeBufferSize_, stIntermediateHeader, stIntermediateMessage, stMessageData, eEncodeFormat_);

    if (eEncoderStatus != STATUS::SUCCESS) { return eEncoderStatus; }

    // Null-terminate the command, if possible. Otherwise, the command will be the size of the buffer.
    if (stMessageData.uiMessageLength < uiEncodeBufferSize_) { stMessageData.pucMessage[stMessageData.uiMessageLength] = '\0'; }
    uiEncodeBufferSize_ = stMessageData.uiMessageLength;

    return STATUS::SUCCESS;
}
