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

#include <algorithm>
#include <cctype>
#include <string_view>
#include <vector>

using namespace novatel::edie;
using namespace novatel::edie::oem;

namespace {

std::string ToUpper(std::string str_)
{
    std::transform(str_.begin(), str_.end(), str_.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return str_;
}

// Split abbreviated ASCII parameters on spaces/commas, keeping quoted strings (and any delimiters inside them) intact.
std::vector<std::string> TokenizeAbbrevAscii(std::string_view sParams_)
{
    std::vector<std::string> vTokens;
    std::string sToken;
    bool bInQuotes = false;
    bool bInToken = false;

    for (const char c : sParams_)
    {
        if (c == '"') { bInQuotes = !bInQuotes; }
        else if (!bInQuotes && (c == ' ' || c == ',' || c == '\t' || c == '\r' || c == '\n'))
        {
            if (bInToken) { vTokens.push_back(std::move(sToken)); }
            sToken.clear();
            bInToken = false;
            continue;
        }
        sToken.push_back(c);
        bInToken = true;
    }
    if (bInToken) { vTokens.push_back(std::move(sToken)); }

    return vTokens;
}

bool IsScalarField(const BaseField& field_) { return field_.type == FIELD_TYPE::SIMPLE || field_.type == FIELD_TYPE::ENUM; }

bool IsValidEnumerator(const BaseField& field_, std::string_view sToken_)
{
    const auto* pclEnumField = dynamic_cast<const EnumField*>(&field_);
    return pclEnumField != nullptr && pclEnumField->enumDef != nullptr && pclEnumField->enumDef->nameValue.count(sToken_) != 0;
}

bool IsOmittablePortField(const BaseField& field_)
{
    const auto* pclEnumField = dynamic_cast<const EnumField*>(&field_);
    return pclEnumField != nullptr && pclEnumField->enumDef != nullptr && pclEnumField->enumDef->name == "PortAddress" &&
           field_.defaultValue.has_value();
}

std::string_view DefaultEnumerator(const BaseField& field_)
{
    const auto& valueName = dynamic_cast<const EnumField&>(field_).enumDef->valueName;
    const auto it = valueName.find(static_cast<uint32_t>(*field_.defaultValue));
    return it != valueName.end() ? it->second : std::string_view{};
}

//----------------------------------------------------------------------------
// Convert abbreviated ASCII command parameters to a full ASCII message body.
//  - Names of enums and messages (%m) are upper-cased, as the receiver is case-insensitive.
//  - An omitted leading port (e.g. "LOG BESTPOSA ONCE") is replaced with its database default.
// Returns false if the parameters can't be mapped onto the message definition.
// uiProvidedFields_ is set to the number of leading top-level fields present in the body.
//----------------------------------------------------------------------------
bool AbbrevAsciiToFullAsciiBody(const FieldInfo& stFieldInfo_, std::vector<std::string> vTokens_, std::string& sBody_, size_t& uiProvidedFields_)
{
    const auto& vFields = stFieldInfo_.messageOrderedFields;
    size_t uiToken = 0;
    size_t uiField = 0;

    for (; uiField < vFields.size() && uiToken < vTokens_.size(); ++uiField)
    {
        const BaseField& field = *vFields[uiField];

        // Arrays and nested fields span a variable number of tokens. Pass the remainder through as-is.
        if (!IsScalarField(field) && field.type != FIELD_TYPE::STRING)
        {
            uiToken = vTokens_.size();
            uiField = vFields.size();
            break;
        }

        std::string& sToken = vTokens_[uiToken];
        if (field.type == FIELD_TYPE::ENUM || field.conversionHash == CalculateBlockCrc32("m")) { sToken = ToUpper(sToken); }

        if (uiField == 0 && vFields.size() > 1 && IsOmittablePortField(field) && !IsValidEnumerator(field, sToken))
        {
            vTokens_.insert(vTokens_.begin() + static_cast<std::ptrdiff_t>(uiToken), std::string(DefaultEnumerator(field)));
        }
        ++uiToken;
    }

    // More parameters than the command accepts
    if (uiToken < vTokens_.size()) { return false; }

    for (size_t i = 0; i < vTokens_.size(); ++i)
    {
        if (i > 0) { sBody_.push_back(OEM4_ASCII_FIELD_SEPARATOR); }
        sBody_ += vTokens_[i];
    }
    sBody_.push_back(OEM4_ASCII_CRC_DELIMITER);

    uiProvidedFields_ = uiField;
    return true;
}

// Fill fields omitted from the end of a command with the database defaults.
bool ApplyDefaults(const FieldInfo& stFieldInfo_, size_t uiFirstField_, CompositeField& clMessage_)
{
    const auto& vFields = stFieldInfo_.messageOrderedFields;
    for (size_t i = uiFirstField_; i < vFields.size(); ++i)
    {
        const BaseField& field = *vFields[i];
        // Required parameter is missing. A message ID never has a meaningful default.
        if (!IsScalarField(field) || !field.defaultValue.has_value() || field.conversionHash == CalculateBlockCrc32("m")) { return false; }

        SimpleTypeVisitor(field, [&](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            clMessage_.SetArrayElement<true>(field, 0, static_cast<T>(*field.defaultValue));
        });
    }
    return true;
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
    clMyMessageDecoder.LoadJsonDb(pclMessageDb_);
    clMyEncoder.LoadJsonDb(pclMessageDb_);
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
    if (pclMyMsgDb == nullptr) { return STATUS::NO_DATABASE; }

    return Encode(*pclMyMsgDb, clMyMessageDecoder, clMyEncoder, pcAbbrevAsciiCommand_, uiAbbrevAsciiCommandLength_, pcEncodeBuffer_,
                  uiEncodeBufferSize_, eEncodeFormat_);
}

// -------------------------------------------------------------------------------------------------------
STATUS Commander::Encode(const MessageDatabase& clJsonDb_, const MessageDecoder& clMessageDecoder_, Encoder& clEncoder_,
                         const char* pcAbbrevAsciiCommand_, const uint32_t uiAbbrevAsciiCommandLength_, char* pcEncodeBuffer_,
                         uint32_t& uiEncodeBufferSize_, const ENCODE_FORMAT eEncodeFormat_)
{
    constexpr uint32_t thisPort = 0xC0;

    if ((pcAbbrevAsciiCommand_ == nullptr) || (pcEncodeBuffer_ == nullptr)) { return STATUS::NULL_PROVIDED; }

    if (eEncodeFormat_ != ENCODE_FORMAT::ASCII && eEncodeFormat_ != ENCODE_FORMAT::BINARY) { return STATUS::UNSUPPORTED; }

    std::vector<std::string> vTokens = TokenizeAbbrevAscii(std::string_view(pcAbbrevAsciiCommand_, uiAbbrevAsciiCommandLength_));
    if (vTokens.empty()) { return STATUS::MALFORMED_INPUT; }

    const std::string strCmdName = ToUpper(vTokens.front());
    vTokens.erase(vTokens.begin());

    MessageDefinition::ConstPtr pclMessageDef = clJsonDb_.GetMsgDef(strCmdName);
    if (!pclMessageDef) { return STATUS::NO_DEFINITION; }

    const auto uiMessageCrc = static_cast<uint32_t>(pclMessageDef->latestMessageCrc);
    const FieldInfo& stFieldInfo = pclMessageDef->GetMsgDefFromCrc(uiMessageCrc);

    std::string strFullAsciiBody;
    size_t uiProvidedFields = 0;
    if (!AbbrevAsciiToFullAsciiBody(stFieldInfo, std::move(vTokens), strFullAsciiBody, uiProvidedFields)) { return STATUS::MALFORMED_INPUT; }

    MessageDataStruct stMessageData;
    MetaDataStruct stMetaData;
    IntermediateHeader stIntermediateHeader;
    CompositeField stIntermediateMessage;

    // Prime the metadata with information we already know
    stMetaData.eFormat = DECODE_FORMAT::ASCII;
    stMetaData.usMessageId = static_cast<uint16_t>(pclMessageDef->logID);
    stMetaData.uiMessageCrc = uiMessageCrc;
    stMetaData.uiLength = static_cast<uint32_t>(strFullAsciiBody.size());

    if (uiProvidedFields > 0)
    {
        const STATUS eDecoderStatus =
            clMessageDecoder_.Decode(reinterpret_cast<const unsigned char*>(strFullAsciiBody.c_str()), stIntermediateMessage, stMetaData);
        if (eDecoderStatus != STATUS::SUCCESS) { return eDecoderStatus; }
    }
    else
    {
        // Nothing to decode, every parameter takes its default
        stIntermediateMessage.resize(stFieldInfo.fixedFieldBytes, stFieldInfo.varFieldCount);
        stIntermediateMessage.SetFieldInfo(pclMessageDef, uiMessageCrc);
    }

    if (!ApplyDefaults(stFieldInfo, uiProvidedFields, stIntermediateMessage)) { return STATUS::MALFORMED_INPUT; }

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
