// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include "stateMachine.hpp"
#include "OutputStateMachineEngine.hpp"
#include "base64.hpp"

#include "ascii.hpp"

using namespace Microsoft::Console;
using namespace Microsoft::Console::VirtualTerminal;

// takes ownership of pDispatch
OutputStateMachineEngine::OutputStateMachineEngine(std::unique_ptr<ITermDispatch> pDispatch) :
    _dispatch(std::move(pDispatch)),
    _pfnFlushToTerminal(nullptr),
    _pTtyConnection(nullptr),
    _lastPrintedChar(AsciiChars::NUL)
{
    THROW_HR_IF_NULL(E_INVALIDARG, _dispatch.get());
}

const ITermDispatch& OutputStateMachineEngine::Dispatch() const noexcept
{
    return *_dispatch;
}

ITermDispatch& OutputStateMachineEngine::Dispatch() noexcept
{
    return *_dispatch;
}

// Routine Description:
// - Triggers the Execute action to indicate that the listener should
//      immediately respond to a C0 control character.
// Arguments:
// - wch - Character to dispatch.
// Return Value:
// - true iff we successfully dispatched the sequence.
bool OutputStateMachineEngine::ActionExecute(const wchar_t wch)
{
    switch (wch)
    {
    case AsciiChars::NUL:
        // microsoft/terminal#1825 - VT applications expect to be able to write NUL
        // and have _nothing_ happen. Filter the NULs here, so they don't fill the
        // buffer with empty spaces.
        break;
    case AsciiChars::BEL:
        _dispatch->WarningBell();
        // microsoft/terminal#2952
        // If we're attached to a terminal, let's also pass the BEL through.
        if (_pfnFlushToTerminal != nullptr)
        {
            _pfnFlushToTerminal();
        }
        break;
    case AsciiChars::BS:
        _dispatch->CursorBackward(1);
        break;
    case AsciiChars::TAB:
        _dispatch->ForwardTab(1);
        break;
    case AsciiChars::CR:
        _dispatch->CarriageReturn();
        break;
    case AsciiChars::LF:
    case AsciiChars::FF:
    case AsciiChars::VT:
        // LF, FF, and VT are identical in function.
        _dispatch->LineFeed(DispatchTypes::LineFeedType::DependsOnMode);
        break;
    case AsciiChars::SI:
        _dispatch->LockingShift(0);
        break;
    case AsciiChars::SO:
        _dispatch->LockingShift(1);
        break;
    default:
        _dispatch->Print(wch);
        break;
    }

    _ClearLastChar();

    return true;
}

// Routine Description:
// - Triggers the Execute action to indicate that the listener should
//      immediately respond to a C0 control character.
// This is called from the Escape state in the state machine, indicating the
//      immediately previous character was an 0x1b. The output state machine
//      does not treat this any differently than a normal ActionExecute.
// Arguments:
// - wch - Character to dispatch.
// Return Value:
// - true iff we successfully dispatched the sequence.
bool OutputStateMachineEngine::ActionExecuteFromEscape(const wchar_t wch)
{
    return ActionExecute(wch);
}

// Routine Description:
// - Triggers the Print action to indicate that the listener should render the
//      character given.
// Arguments:
// - wch - Character to dispatch.
// Return Value:
// - true iff we successfully dispatched the sequence.
bool OutputStateMachineEngine::ActionPrint(const wchar_t wch)
{
    // Stash the last character of the string, if it's a graphical character
    if (wch >= AsciiChars::SPC)
    {
        _lastPrintedChar = wch;
    }

    _dispatch->Print(wch); // call print

    return true;
}

// Routine Description:
// - Triggers the Print action to indicate that the listener should render the
//      string of characters given.
// Arguments:
// - string - string to dispatch.
// Return Value:
// - true iff we successfully dispatched the sequence.
bool OutputStateMachineEngine::ActionPrintString(const std::wstring_view string)
{
    if (string.empty())
    {
        return true;
    }

    // Stash the last character of the string, if it's a graphical character
    const wchar_t wch = string.back();
    if (wch >= AsciiChars::SPC)
    {
        _lastPrintedChar = wch;
    }

    _dispatch->PrintString(string); // call print

    return true;
}

// Routine Description:
// This is called when we have determined that we don't understand a particular
//      sequence, or the adapter has determined that the string is intended for
//      the actual terminal (when we're acting as a pty).
// - Pass the string through to the target terminal application. If we're a pty,
//      then we'll have a TerminalConnection that we'll write the string to.
//      Otherwise, we're the terminal device, and we'll eat the string (because
//      we don't know what to do with it)
// Arguments:
// - string - string to dispatch.
// Return Value:
// - true iff we successfully dispatched the sequence.
bool OutputStateMachineEngine::ActionPassThroughString(const std::wstring_view string)
{
    bool success = true;
    if (_pTtyConnection != nullptr)
    {
        const auto hr = _pTtyConnection->WriteTerminalW(string);
        LOG_IF_FAILED(hr);
        success = SUCCEEDED(hr);
    }
    // If there's not a TTY connection, our previous behavior was to eat the string.

    return success;
}

// Routine Description:
// - Triggers the EscDispatch action to indicate that the listener should handle
//      a simple escape sequence. These sequences traditionally start with ESC
//      and a simple letter. No complicated parameters.
// Arguments:
// - id - Identifier of the escape sequence to dispatch.
// Return Value:
// - true iff we successfully dispatched the sequence.
bool OutputStateMachineEngine::ActionEscDispatch(const VTID id)
{
    bool success = false;

    switch (id)
    {
    case EscActionCodes::ST_StringTerminator:
        // This is the 7-bit string terminator, which is essentially a no-op.
        success = true;
        break;
    case EscActionCodes::DECSC_CursorSave:
        success = _dispatch->CursorSaveState();
        TermTelemetry::Instance().Log(TermTelemetry::Codes::DECSC);
        break;
    case EscActionCodes::DECRC_CursorRestore:
        success = _dispatch->CursorRestoreState();
        TermTelemetry::Instance().Log(TermTelemetry::Codes::DECRC);
        break;
    case EscActionCodes::DECKPAM_KeypadApplicationMode:
        success = _dispatch->SetKeypadMode(true);
        TermTelemetry::Instance().Log(TermTelemetry::Codes::DECKPAM);
        break;
    case EscActionCodes::DECKPNM_KeypadNumericMode:
        success = _dispatch->SetKeypadMode(false);
        TermTelemetry::Instance().Log(TermTelemetry::Codes::DECKPNM);
        break;
    case EscActionCodes::NEL_NextLine:
        success = _dispatch->LineFeed(DispatchTypes::LineFeedType::WithReturn);
        TermTelemetry::Instance().Log(TermTelemetry::Codes::NEL);
        break;
    case EscActionCodes::IND_Index:
        success = _dispatch->LineFeed(DispatchTypes::LineFeedType::WithoutReturn);
        TermTelemetry::Instance().Log(TermTelemetry::Codes::IND);
        break;
    case EscActionCodes::RI_ReverseLineFeed:
        success = _dispatch->ReverseLineFeed();
        TermTelemetry::Instance().Log(TermTelemetry::Codes::RI);
        break;
    case EscActionCodes::HTS_HorizontalTabSet:
        success = _dispatch->HorizontalTabSet();
        TermTelemetry::Instance().Log(TermTelemetry::Codes::HTS);
        break;
    case EscActionCodes::RIS_ResetToInitialState:
        success = _dispatch->HardReset();
        TermTelemetry::Instance().Log(TermTelemetry::Codes::RIS);
        break;
    case EscActionCodes::SS2_SingleShift:
        success = _dispatch->SingleShift(2);
        TermTelemetry::Instance().Log(TermTelemetry::Codes::SS2);
        break;
    case EscActionCodes::SS3_SingleShift:
        success = _dispatch->SingleShift(3);
        TermTelemetry::Instance().Log(TermTelemetry::Codes::SS3);
        break;
    case EscActionCodes::LS2_LockingShift:
        success = _dispatch->LockingShift(2);
        TermTelemetry::Instance().Log(TermTelemetry::Codes::LS2);
        break;
    case EscActionCodes::LS3_LockingShift:
        success = _dispatch->LockingShift(3);
        TermTelemetry::Instance().Log(TermTelemetry::Codes::LS3);
        break;
    case EscActionCodes::LS1R_LockingShift:
        success = _dispatch->LockingShiftRight(1);
        TermTelemetry::Instance().Log(TermTelemetry::Codes::LS1R);
        break;
    case EscActionCodes::LS2R_LockingShift:
        success = _dispatch->LockingShiftRight(2);
        TermTelemetry::Instance().Log(TermTelemetry::Codes::LS2R);
        break;
    case EscActionCodes::LS3R_LockingShift:
        success = _dispatch->LockingShiftRight(3);
        TermTelemetry::Instance().Log(TermTelemetry::Codes::LS3R);
        break;
    case EscActionCodes::DECALN_ScreenAlignmentPattern:
        success = _dispatch->ScreenAlignmentPattern();
        TermTelemetry::Instance().Log(TermTelemetry::Codes::DECALN);
        break;
    default:
        const auto commandChar = id[0];
        const auto commandParameter = id.SubSequence(1);
        switch (commandChar)
        {
        case '%':
            success = _dispatch->DesignateCodingSystem(commandParameter);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::DOCS);
            break;
        case '(':
            success = _dispatch->Designate94Charset(0, commandParameter);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::DesignateG0);
            break;
        case ')':
            success = _dispatch->Designate94Charset(1, commandParameter);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::DesignateG1);
            break;
        case '*':
            success = _dispatch->Designate94Charset(2, commandParameter);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::DesignateG2);
            break;
        case '+':
            success = _dispatch->Designate94Charset(3, commandParameter);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::DesignateG3);
            break;
        case '-':
            success = _dispatch->Designate96Charset(1, commandParameter);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::DesignateG1);
            break;
        case '.':
            success = _dispatch->Designate96Charset(2, commandParameter);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::DesignateG2);
            break;
        case '/':
            success = _dispatch->Designate96Charset(3, commandParameter);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::DesignateG3);
            break;
        default:
            // If no functions to call, overall dispatch was a failure.
            success = false;
            break;
        }
    }

    // If we were unable to process the string, and there's a TTY attached to us,
    //      trigger the state machine to flush the string to the terminal.
    if (_pfnFlushToTerminal != nullptr && !success)
    {
        success = _pfnFlushToTerminal();
    }

    _ClearLastChar();

    return success;
}

// Method Description:
// - Triggers the Vt52EscDispatch action to indicate that the listener should handle
//      a VT52 escape sequence. These sequences start with ESC and a single letter,
//      sometimes followed by parameters.
// Arguments:
// - id - Identifier of the VT52 sequence to dispatch.
// - parameters - Set of parameters collected while parsing the sequence.
// Return Value:
// - true iff we successfully dispatched the sequence.
bool OutputStateMachineEngine::ActionVt52EscDispatch(const VTID id, const gsl::span<const size_t> parameters)
{
    bool success = false;

    switch (id)
    {
    case Vt52ActionCodes::CursorUp:
        success = _dispatch->CursorUp(1);
        break;
    case Vt52ActionCodes::CursorDown:
        success = _dispatch->CursorDown(1);
        break;
    case Vt52ActionCodes::CursorRight:
        success = _dispatch->CursorForward(1);
        break;
    case Vt52ActionCodes::CursorLeft:
        success = _dispatch->CursorBackward(1);
        break;
    case Vt52ActionCodes::EnterGraphicsMode:
        success = _dispatch->Designate94Charset(0, DispatchTypes::CharacterSets::DecSpecialGraphics);
        break;
    case Vt52ActionCodes::ExitGraphicsMode:
        success = _dispatch->Designate94Charset(0, DispatchTypes::CharacterSets::ASCII);
        break;
    case Vt52ActionCodes::CursorToHome:
        success = _dispatch->CursorPosition(1, 1);
        break;
    case Vt52ActionCodes::ReverseLineFeed:
        success = _dispatch->ReverseLineFeed();
        break;
    case Vt52ActionCodes::EraseToEndOfScreen:
        success = _dispatch->EraseInDisplay(DispatchTypes::EraseType::ToEnd);
        break;
    case Vt52ActionCodes::EraseToEndOfLine:
        success = _dispatch->EraseInLine(DispatchTypes::EraseType::ToEnd);
        break;
    case Vt52ActionCodes::DirectCursorAddress:
        // VT52 cursor addresses are provided as ASCII characters, with
        // the lowest value being a space, representing an address of 1.
        success = _dispatch->CursorPosition(gsl::at(parameters, 0) - ' ' + 1, gsl::at(parameters, 1) - ' ' + 1);
        break;
    case Vt52ActionCodes::Identify:
        success = _dispatch->Vt52DeviceAttributes();
        break;
    case Vt52ActionCodes::EnterAlternateKeypadMode:
        success = _dispatch->SetKeypadMode(true);
        break;
    case Vt52ActionCodes::ExitAlternateKeypadMode:
        success = _dispatch->SetKeypadMode(false);
        break;
    case Vt52ActionCodes::ExitVt52Mode:
    {
        const DispatchTypes::PrivateModeParams mode[] = { DispatchTypes::PrivateModeParams::DECANM_AnsiMode };
        success = _dispatch->SetPrivateModes(mode);
        break;
    }
    default:
        // If no functions to call, overall dispatch was a failure.
        success = false;
        break;
    }

    _ClearLastChar();

    return success;
}

// Routine Description:
// - Triggers the CsiDispatch action to indicate that the listener should handle
//      a control sequence. These sequences perform various API-type commands
//      that can include many parameters.
// Arguments:
// - id - Identifier of the control sequence to dispatch.
// - parameters - set of numeric parameters collected while parsing the sequence.
// Return Value:
// - true iff we successfully dispatched the sequence.
bool OutputStateMachineEngine::ActionCsiDispatch(const VTID id, gsl::span<const size_t> parameters)
{
    bool success = false;
    size_t distance = 0;
    size_t line = 0;
    size_t column = 0;
    size_t topMargin = 0;
    size_t bottomMargin = 0;
    size_t numTabs = 0;
    size_t clearType = 0;
    unsigned int function = 0;
    DispatchTypes::EraseType eraseType = DispatchTypes::EraseType::ToEnd;
    std::vector<DispatchTypes::PrivateModeParams> privateModeParams;
    // We hold the vector in the class because client applications that do a lot of color work
    // would spend a lot of time reallocating/resizing the vector.
    _graphicsOptions.clear();
    DispatchTypes::AnsiStatusType deviceStatusType = static_cast<DispatchTypes::AnsiStatusType>(0); // there is no default status type.
    size_t repeatCount = 0;
    DispatchTypes::CursorStyle cursorStyle = DefaultCursorStyle;
    // This is all the args after the first arg, and the count of args not including the first one.
    const auto remainingParams = parameters.size() > 1 ? parameters.subspan(1) : gsl::span<const size_t>{};

    // fill params
    switch (id)
    {
    case CsiActionCodes::CUU_CursorUp:
    case CsiActionCodes::CUD_CursorDown:
    case CsiActionCodes::CUF_CursorForward:
    case CsiActionCodes::CUB_CursorBackward:
    case CsiActionCodes::CNL_CursorNextLine:
    case CsiActionCodes::CPL_CursorPrevLine:
    case CsiActionCodes::CHA_CursorHorizontalAbsolute:
    case CsiActionCodes::HPA_HorizontalPositionAbsolute:
    case CsiActionCodes::VPA_VerticalLinePositionAbsolute:
    case CsiActionCodes::HPR_HorizontalPositionRelative:
    case CsiActionCodes::VPR_VerticalPositionRelative:
    case CsiActionCodes::ICH_InsertCharacter:
    case CsiActionCodes::DCH_DeleteCharacter:
    case CsiActionCodes::ECH_EraseCharacters:
        success = _GetCursorDistance(parameters, distance);
        break;
    case CsiActionCodes::HVP_HorizontalVerticalPosition:
    case CsiActionCodes::CUP_CursorPosition:
        success = _GetXYPosition(parameters, line, column);
        break;
    case CsiActionCodes::DECSTBM_SetScrollingRegion:
        success = _GetTopBottomMargins(parameters, topMargin, bottomMargin);
        break;
    case CsiActionCodes::ED_EraseDisplay:
    case CsiActionCodes::EL_EraseLine:
        success = _GetEraseOperation(parameters, eraseType);
        break;
    case CsiActionCodes::DECSET_PrivateModeSet:
    case CsiActionCodes::DECRST_PrivateModeReset:
        success = _GetPrivateModeParams(parameters, privateModeParams);
        break;
    case CsiActionCodes::SGR_SetGraphicsRendition:
        success = _GetGraphicsOptions(parameters, _graphicsOptions);
        break;
    case CsiActionCodes::DSR_DeviceStatusReport:
        success = _GetDeviceStatusOperation(parameters, deviceStatusType);
        break;
    case CsiActionCodes::DA_DeviceAttributes:
    case CsiActionCodes::DA2_SecondaryDeviceAttributes:
    case CsiActionCodes::DA3_TertiaryDeviceAttributes:
        success = _VerifyDeviceAttributesParams(parameters);
        break;
    case CsiActionCodes::SU_ScrollUp:
    case CsiActionCodes::SD_ScrollDown:
        success = _GetScrollDistance(parameters, distance);
        break;
    case CsiActionCodes::ANSISYSSC_CursorSave:
    case CsiActionCodes::ANSISYSRC_CursorRestore:
        success = _VerifyHasNoParameters(parameters);
        break;
    case CsiActionCodes::IL_InsertLine:
    case CsiActionCodes::DL_DeleteLine:
        success = _GetScrollDistance(parameters, distance);
        break;
    case CsiActionCodes::CHT_CursorForwardTab:
    case CsiActionCodes::CBT_CursorBackTab:
        success = _GetTabDistance(parameters, numTabs);
        break;
    case CsiActionCodes::TBC_TabClear:
        success = _GetTabClearType(parameters, clearType);
        break;
    case CsiActionCodes::DTTERM_WindowManipulation:
        success = _GetWindowManipulationType(parameters, function);
        break;
    case CsiActionCodes::REP_RepeatCharacter:
        success = _GetRepeatCount(parameters, repeatCount);
        break;
    case CsiActionCodes::DECSCUSR_SetCursorStyle:
        success = _GetCursorStyle(parameters, cursorStyle);
        break;
    default:
        // If no params to fill, param filling was successful.
        success = true;
        break;
    }

    // if param filling successful, try to dispatch
    if (success)
    {
        switch (id)
        {
        case CsiActionCodes::CUU_CursorUp:
            success = _dispatch->CursorUp(distance);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::CUU);
            break;
        case CsiActionCodes::CUD_CursorDown:
            success = _dispatch->CursorDown(distance);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::CUD);
            break;
        case CsiActionCodes::CUF_CursorForward:
            success = _dispatch->CursorForward(distance);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::CUF);
            break;
        case CsiActionCodes::CUB_CursorBackward:
            success = _dispatch->CursorBackward(distance);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::CUB);
            break;
        case CsiActionCodes::CNL_CursorNextLine:
            success = _dispatch->CursorNextLine(distance);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::CNL);
            break;
        case CsiActionCodes::CPL_CursorPrevLine:
            success = _dispatch->CursorPrevLine(distance);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::CPL);
            break;
        case CsiActionCodes::CHA_CursorHorizontalAbsolute:
        case CsiActionCodes::HPA_HorizontalPositionAbsolute:
            success = _dispatch->CursorHorizontalPositionAbsolute(distance);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::CHA);
            break;
        case CsiActionCodes::VPA_VerticalLinePositionAbsolute:
            success = _dispatch->VerticalLinePositionAbsolute(distance);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::VPA);
            break;
        case CsiActionCodes::HPR_HorizontalPositionRelative:
            success = _dispatch->HorizontalPositionRelative(distance);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::HPR);
            break;
        case CsiActionCodes::VPR_VerticalPositionRelative:
            success = _dispatch->VerticalPositionRelative(distance);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::VPR);
            break;
        case CsiActionCodes::CUP_CursorPosition:
        case CsiActionCodes::HVP_HorizontalVerticalPosition:
            success = _dispatch->CursorPosition(line, column);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::CUP);
            break;
        case CsiActionCodes::DECSTBM_SetScrollingRegion:
            success = _dispatch->SetTopBottomScrollingMargins(topMargin, bottomMargin);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::DECSTBM);
            break;
        case CsiActionCodes::ICH_InsertCharacter:
            success = _dispatch->InsertCharacter(distance);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::ICH);
            break;
        case CsiActionCodes::DCH_DeleteCharacter:
            success = _dispatch->DeleteCharacter(distance);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::DCH);
            break;
        case CsiActionCodes::ED_EraseDisplay:
            success = _dispatch->EraseInDisplay(eraseType);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::ED);
            break;
        case CsiActionCodes::EL_EraseLine:
            success = _dispatch->EraseInLine(eraseType);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::EL);
            break;
        case CsiActionCodes::DECSET_PrivateModeSet:
            success = _dispatch->SetPrivateModes({ privateModeParams.data(), privateModeParams.size() });
            //TODO: MSFT:6367459 Add specific logging for each of the DECSET/DECRST codes
            TermTelemetry::Instance().Log(TermTelemetry::Codes::DECSET);
            break;
        case CsiActionCodes::DECRST_PrivateModeReset:
            success = _dispatch->ResetPrivateModes({ privateModeParams.data(), privateModeParams.size() });
            TermTelemetry::Instance().Log(TermTelemetry::Codes::DECRST);
            break;
        case CsiActionCodes::SGR_SetGraphicsRendition:
            success = _dispatch->SetGraphicsRendition({ _graphicsOptions.data(), _graphicsOptions.size() });
            TermTelemetry::Instance().Log(TermTelemetry::Codes::SGR);
            break;
        case CsiActionCodes::DSR_DeviceStatusReport:
            success = _dispatch->DeviceStatusReport(deviceStatusType);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::DSR);
            break;
        case CsiActionCodes::DA_DeviceAttributes:
            success = _dispatch->DeviceAttributes();
            TermTelemetry::Instance().Log(TermTelemetry::Codes::DA);
            break;
        case CsiActionCodes::DA2_SecondaryDeviceAttributes:
            success = _dispatch->SecondaryDeviceAttributes();
            TermTelemetry::Instance().Log(TermTelemetry::Codes::DA2);
            break;
        case CsiActionCodes::DA3_TertiaryDeviceAttributes:
            success = _dispatch->TertiaryDeviceAttributes();
            TermTelemetry::Instance().Log(TermTelemetry::Codes::DA3);
            break;
        case CsiActionCodes::SU_ScrollUp:
            success = _dispatch->ScrollUp(distance);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::SU);
            break;
        case CsiActionCodes::SD_ScrollDown:
            success = _dispatch->ScrollDown(distance);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::SD);
            break;
        case CsiActionCodes::ANSISYSSC_CursorSave:
            success = _dispatch->CursorSaveState();
            TermTelemetry::Instance().Log(TermTelemetry::Codes::ANSISYSSC);
            break;
        case CsiActionCodes::ANSISYSRC_CursorRestore:
            success = _dispatch->CursorRestoreState();
            TermTelemetry::Instance().Log(TermTelemetry::Codes::ANSISYSRC);
            break;
        case CsiActionCodes::IL_InsertLine:
            success = _dispatch->InsertLine(distance);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::IL);
            break;
        case CsiActionCodes::DL_DeleteLine:
            success = _dispatch->DeleteLine(distance);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::DL);
            break;
        case CsiActionCodes::CHT_CursorForwardTab:
            success = _dispatch->ForwardTab(numTabs);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::CHT);
            break;
        case CsiActionCodes::CBT_CursorBackTab:
            success = _dispatch->BackwardsTab(numTabs);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::CBT);
            break;
        case CsiActionCodes::TBC_TabClear:
            success = _dispatch->TabClear(clearType);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::TBC);
            break;
        case CsiActionCodes::ECH_EraseCharacters:
            success = _dispatch->EraseCharacters(distance);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::ECH);
            break;
        case CsiActionCodes::DTTERM_WindowManipulation:
            success = _dispatch->WindowManipulation(static_cast<DispatchTypes::WindowManipulationType>(function),
                                                    remainingParams);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::DTTERM_WM);
            break;
        case CsiActionCodes::REP_RepeatCharacter:
            // Handled w/o the dispatch. This function is unique in that way
            // If this were in the ITerminalDispatch, then each
            // implementation would effectively be the same, calling only
            // functions that are already part of the interface.
            // Print the last graphical character a number of times.
            if (_lastPrintedChar != AsciiChars::NUL)
            {
                std::wstring wstr(repeatCount, _lastPrintedChar);
                _dispatch->PrintString(wstr);
            }
            success = true;
            TermTelemetry::Instance().Log(TermTelemetry::Codes::REP);
            break;
        case CsiActionCodes::DECSCUSR_SetCursorStyle:
            success = _dispatch->SetCursorStyle(cursorStyle);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::DECSCUSR);
            break;
        case CsiActionCodes::DECSTR_SoftReset:
            success = _dispatch->SoftReset();
            TermTelemetry::Instance().Log(TermTelemetry::Codes::DECSTR);
            break;
        default:
            // If no functions to call, overall dispatch was a failure.
            success = false;
            break;
        }
    }

    // If we were unable to process the string, and there's a TTY attached to us,
    //      trigger the state machine to flush the string to the terminal.
    if (_pfnFlushToTerminal != nullptr && !success)
    {
        success = _pfnFlushToTerminal();
    }

    _ClearLastChar();

    return success;
}

// Routine Description:
// - Triggers the Clear action to indicate that the state machine should erase
//      all internal state.
// Arguments:
// - <none>
// Return Value:
// - <none>
bool OutputStateMachineEngine::ActionClear() noexcept
{
    // do nothing.
    return true;
}

// Routine Description:
// - Triggers the Ignore action to indicate that the state machine should eat
//      this character and say nothing.
// Arguments:
// - <none>
// Return Value:
// - <none>
bool OutputStateMachineEngine::ActionIgnore() noexcept
{
    // do nothing.
    return true;
}

// Routine Description:
// - Triggers the OscDispatch action to indicate that the listener should handle a control sequence.
//   These sequences perform various API-type commands that can include many parameters.
// Arguments:
// - wch - Character to dispatch. This will be a BEL or ST char.
// - parameter - identifier of the OSC action to perform
// - string - OSC string we've collected. NOT null terminated.
// Return Value:
// - true if we handled the dispatch.
bool OutputStateMachineEngine::ActionOscDispatch(const wchar_t /*wch*/,
                                                 const size_t parameter,
                                                 const std::wstring_view string)
{
    bool success = false;
    std::wstring title;
    std::wstring setClipboardContent;
    std::wstring params;
    std::wstring uri;
    bool queryClipboard = false;
    size_t tableIndex = 0;
    DWORD color = 0;

    switch (parameter)
    {
    case OscActionCodes::SetIconAndWindowTitle:
    case OscActionCodes::SetWindowIcon:
    case OscActionCodes::SetWindowTitle:
        success = _GetOscTitle(string, title);
        break;
    case OscActionCodes::SetColor:
        success = _GetOscSetColorTable(string, tableIndex, color);
        break;
    case OscActionCodes::SetForegroundColor:
    case OscActionCodes::SetBackgroundColor:
    case OscActionCodes::SetCursorColor:
        success = _GetOscSetColor(string, color);
        break;
    case OscActionCodes::SetClipboard:
        success = _GetOscSetClipboard(string, setClipboardContent, queryClipboard);
        break;
    case OscActionCodes::ResetCursorColor:
        // the console uses 0xffffffff as an "invalid color" value
        color = 0xffffffff;
        success = true;
        break;
    case OscActionCodes::Hyperlink:
        success = _ParseHyperlink(string, params, uri);
        break;
    default:
        // If no functions to call, overall dispatch was a failure.
        success = false;
        break;
    }
    if (success)
    {
        switch (parameter)
        {
        case OscActionCodes::SetIconAndWindowTitle:
        case OscActionCodes::SetWindowIcon:
        case OscActionCodes::SetWindowTitle:
            success = _dispatch->SetWindowTitle(title);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::OSCWT);
            break;
        case OscActionCodes::SetColor:
            success = _dispatch->SetColorTableEntry(tableIndex, color);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::OSCCT);
            break;
        case OscActionCodes::SetForegroundColor:
            success = _dispatch->SetDefaultForeground(color);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::OSCFG);
            break;
        case OscActionCodes::SetBackgroundColor:
            success = _dispatch->SetDefaultBackground(color);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::OSCBG);
            break;
        case OscActionCodes::SetCursorColor:
            success = _dispatch->SetCursorColor(color);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::OSCSCC);
            break;
        case OscActionCodes::SetClipboard:
            if (!queryClipboard)
            {
                success = _dispatch->SetClipboard(setClipboardContent);
            }
            TermTelemetry::Instance().Log(TermTelemetry::Codes::OSCSCB);
            break;
        case OscActionCodes::ResetCursorColor:
            success = _dispatch->SetCursorColor(color);
            TermTelemetry::Instance().Log(TermTelemetry::Codes::OSCRCC);
            break;
        case OscActionCodes::Hyperlink:
            if (uri.empty())
            {
                success = _dispatch->EndHyperlink();
            }
            else
            {
                success = _dispatch->AddHyperlink(uri, params);
            }
            break;
        default:
            // If no functions to call, overall dispatch was a failure.
            success = false;
            break;
        }
    }

    // If we were unable to process the string, and there's a TTY attached to us,
    //      trigger the state machine to flush the string to the terminal.
    if (_pfnFlushToTerminal != nullptr && !success)
    {
        success = _pfnFlushToTerminal();
    }

    _ClearLastChar();

    return success;
}

// Routine Description:
// - Triggers the Ss3Dispatch action to indicate that the listener should handle
//      a control sequence. These sequences perform various API-type commands
//      that can include many parameters.
// Arguments:
// - wch - Character to dispatch.
// - parameters - set of numeric parameters collected while parsing the sequence.
// Return Value:
// - true iff we successfully dispatched the sequence.
bool OutputStateMachineEngine::ActionSs3Dispatch(const wchar_t /*wch*/,
                                                 const gsl::span<const size_t> /*parameters*/) noexcept
{
    // The output engine doesn't handle any SS3 sequences.
    _ClearLastChar();
    return false;
}

// Routine Description:
// - Retrieves the listed graphics options to be applied in order to the "font style" of the next characters inserted into the buffer.
// Arguments:
// - parameters - The parameters to parse
// - options - Space that will be filled with valid options from the GraphicsOptions enum
// Return Value:
// - True if we successfully retrieved an array of valid graphics options from the parameters we've stored. False otherwise.
bool OutputStateMachineEngine::_GetGraphicsOptions(const gsl::span<const size_t> parameters,
                                                   std::vector<DispatchTypes::GraphicsOptions>& options) const
{
    bool success = false;

    if (parameters.empty())
    {
        options.push_back(DefaultGraphicsOption);
        success = true;
    }
    else
    {
        for (const auto& p : parameters)
        {
            options.push_back((DispatchTypes::GraphicsOptions)p);
        }
        success = true;
    }

    // If we were unable to process the string, and there's a TTY attached to us,
    //      trigger the state machine to flush the string to the terminal.
    if (_pfnFlushToTerminal != nullptr && !success)
    {
        success = _pfnFlushToTerminal();
    }

    return success;
}

// Routine Description:
// - Retrieves the erase type parameter for an upcoming operation.
// Arguments:
// - parameters - The parameters to parse
// - eraseType - Receives the erase type parameter
// Return Value:
// - True if we successfully pulled an erase type from the parameters we've stored. False otherwise.
bool OutputStateMachineEngine::_GetEraseOperation(const gsl::span<const size_t> parameters,
                                                  DispatchTypes::EraseType& eraseType) const noexcept
{
    bool success = false; // If we have too many parameters or don't know what to do with the given value, return false.
    eraseType = DefaultEraseType; // if we fail, just put the default type in.

    if (parameters.empty())
    {
        // Empty parameter sequences should use the default
        eraseType = DefaultEraseType;
        success = true;
    }
    else if (parameters.size() == 1)
    {
        // If there's one parameter, attempt to match it to the values we accept.
        const auto param = static_cast<DispatchTypes::EraseType>(til::at(parameters, 0));

        switch (param)
        {
        case DispatchTypes::EraseType::ToEnd:
        case DispatchTypes::EraseType::FromBeginning:
        case DispatchTypes::EraseType::All:
        case DispatchTypes::EraseType::Scrollback:
            eraseType = param;
            success = true;
            break;
        }
    }

    return success;
}

// Routine Description:
// - Retrieves a distance for a cursor operation from the parameter pool stored during Param actions.
// Arguments:
// - parameters - The parameters to parse
// - distance - Receives the distance
// Return Value:
// - True if we successfully pulled the cursor distance from the parameters we've stored. False otherwise.
bool OutputStateMachineEngine::_GetCursorDistance(const gsl::span<const size_t> parameters,
                                                  size_t& distance) const noexcept
{
    bool success = false;
    distance = DefaultCursorDistance;

    if (parameters.empty())
    {
        // Empty parameter sequences should use the default
        success = true;
    }
    else if (parameters.size() == 1)
    {
        // If there's one parameter, use it.
        distance = til::at(parameters, 0);
        success = true;
    }

    // Distances of 0 should be changed to 1.
    if (distance == 0)
    {
        distance = DefaultCursorDistance;
    }

    return success;
}

// Routine Description:
// - Retrieves a distance for a scroll operation from the parameter pool stored during Param actions.
// Arguments:
// - parameters - The parameters to parse
// - distance - Receives the distance
// Return Value:
// - True if we successfully pulled the scroll distance from the parameters we've stored. False otherwise.
bool OutputStateMachineEngine::_GetScrollDistance(const gsl::span<const size_t> parameters,
                                                  size_t& distance) const noexcept
{
    bool success = false;
    distance = DefaultScrollDistance;

    if (parameters.empty())
    {
        // Empty parameter sequences should use the default
        success = true;
    }
    else if (parameters.size() == 1)
    {
        // If there's one parameter, use it.
        distance = til::at(parameters, 0);
        success = true;
    }

    // Distances of 0 should be changed to 1.
    if (distance == 0)
    {
        distance = DefaultScrollDistance;
    }

    return success;
}

// Routine Description:
// - Retrieves a width for the console window from the parameter pool stored during Param actions.
// Arguments:
// - parameters - The parameters to parse
// - consoleWidth - Receives the width
// Return Value:
// - True if we successfully pulled the width from the parameters we've stored. False otherwise.
bool OutputStateMachineEngine::_GetConsoleWidth(const gsl::span<const size_t> parameters,
                                                size_t& consoleWidth) const noexcept
{
    bool success = false;
    consoleWidth = DefaultConsoleWidth;

    if (parameters.empty())
    {
        // Empty parameter sequences should use the default
        success = true;
    }
    else if (parameters.size() == 1)
    {
        // If there's one parameter, use it.
        consoleWidth = til::at(parameters, 0);
        success = true;
    }

    // Distances of 0 should be changed to 80.
    if (consoleWidth == 0)
    {
        consoleWidth = DefaultConsoleWidth;
    }

    return success;
}

// Routine Description:
// - Retrieves an X/Y coordinate pair for a cursor operation from the parameter pool stored during Param actions.
// Arguments:
// - parameters - The parameters to parse
// - line - Receives the Y/Line/Row position
// - column - Receives the X/Column position
// Return Value:
// - True if we successfully pulled the cursor coordinates from the parameters we've stored. False otherwise.
bool OutputStateMachineEngine::_GetXYPosition(const gsl::span<const size_t> parameters,
                                              size_t& line,
                                              size_t& column) const noexcept
{
    bool success = false;
    line = DefaultLine;
    column = DefaultColumn;

    if (parameters.empty())
    {
        // Empty parameter sequences should use the default
        success = true;
    }
    else if (parameters.size() == 1)
    {
        // If there's only one param, leave the default for the column, and retrieve the specified row.
        line = til::at(parameters, 0);
        success = true;
    }
    else if (parameters.size() == 2)
    {
        // If there are exactly two parameters, use them.
        line = til::at(parameters, 0);
        column = til::at(parameters, 1);
        success = true;
    }

    // Distances of 0 should be changed to 1.
    if (line == 0)
    {
        line = DefaultLine;
    }

    if (column == 0)
    {
        column = DefaultColumn;
    }

    return success;
}

// Routine Description:
// - Retrieves a top and bottom pair for setting the margins from the parameter pool stored during Param actions
// Arguments:
// - parameters - The parameters to parse
// - topMargin - Receives the top margin
// - bottomMargin - Receives the bottom margin
// Return Value:
// - True if we successfully pulled the margin settings from the parameters we've stored. False otherwise.
bool OutputStateMachineEngine::_GetTopBottomMargins(const gsl::span<const size_t> parameters,
                                                    size_t& topMargin,
                                                    size_t& bottomMargin) const noexcept
{
    // Notes:                           (input -> state machine out)
    // having only a top param is legal         ([3;r   -> 3,0)
    // having only a bottom param is legal      ([;3r   -> 0,3)
    // having neither uses the defaults         ([;r [r -> 0,0)
    // an illegal combo (eg, 3;2r) is ignored

    bool success = false;
    topMargin = DefaultTopMargin;
    bottomMargin = DefaultBottomMargin;

    if (parameters.empty())
    {
        // Empty parameter sequences should use the default
        success = true;
    }
    else if (parameters.size() == 1)
    {
        topMargin = til::at(parameters, 0);
        success = true;
    }
    else if (parameters.size() == 2)
    {
        // If there are exactly two parameters, use them.
        topMargin = til::at(parameters, 0);
        bottomMargin = til::at(parameters, 1);
        success = true;
    }

    if (bottomMargin > 0 && bottomMargin < topMargin)
    {
        success = false;
    }
    return success;
}
// Routine Description:
// - Retrieves the status type parameter for an upcoming device query operation
// Arguments:
// - parameters - The parameters to parse
// - statusType - Receives the Status Type parameter
// Return Value:
// - True if we successfully found a device operation in the parameters stored. False otherwise.
bool OutputStateMachineEngine::_GetDeviceStatusOperation(const gsl::span<const size_t> parameters,
                                                         DispatchTypes::AnsiStatusType& statusType) const noexcept
{
    bool success = false;
    statusType = static_cast<DispatchTypes::AnsiStatusType>(0);

    if (parameters.size() == 1)
    {
        // If there's one parameter, attempt to match it to the values we accept.
        const auto param = til::at(parameters, 0);

        switch (param)
        {
        // This looks kinda silly, but I want the parser to reject (success = false) any status types we haven't put here.
        case (unsigned short)DispatchTypes::AnsiStatusType::OS_OperatingStatus:
            statusType = DispatchTypes::AnsiStatusType::OS_OperatingStatus;
            success = true;
            break;
        case (unsigned short)DispatchTypes::AnsiStatusType::CPR_CursorPositionReport:
            statusType = DispatchTypes::AnsiStatusType::CPR_CursorPositionReport;
            success = true;
            break;
        default:
            success = false;
            break;
        }
    }

    return success;
}

// Routine Description:
// - Retrieves the listed private mode params be set/reset by DECSET/DECRST
// Arguments:
// - parameters - The parameters to parse
// - privateModes - Space that will be filled with valid params from the PrivateModeParams enum
// Return Value:
// - True if we successfully retrieved an array of private mode params from the parameters we've stored. False otherwise.
bool OutputStateMachineEngine::_GetPrivateModeParams(const gsl::span<const size_t> parameters,
                                                     std::vector<DispatchTypes::PrivateModeParams>& privateModes) const
{
    bool success = false;
    // Can't just set nothing at all
    if (parameters.size() > 0)
    {
        for (const auto& p : parameters)
        {
            privateModes.push_back((DispatchTypes::PrivateModeParams)p);
        }
        success = true;
    }
    return success;
}

// - Verifies that no parameters were parsed for the current CSI sequence
// Arguments:
// - parameters - The parameters to parse
// Return Value:
// - True if there were no parameters. False otherwise.
bool OutputStateMachineEngine::_VerifyHasNoParameters(const gsl::span<const size_t> parameters) const noexcept
{
    return parameters.empty();
}

// Routine Description:
// - Validates that we received the correct parameter sequence for the Device Attributes command.
// - For DA, we should have received either NO parameters or just one 0 parameter. Anything else is not acceptable.
// Arguments:
// - parameters - The parameters to parse
// Return Value:
// - True if the DA params were valid. False otherwise.
bool OutputStateMachineEngine::_VerifyDeviceAttributesParams(const gsl::span<const size_t> parameters) const noexcept
{
    bool success = false;

    if (parameters.empty())
    {
        success = true;
    }
    else if (parameters.size() == 1)
    {
        if (til::at(parameters, 0) == 0)
        {
            success = true;
        }
    }

    return success;
}

// Routine Description:
// - Null terminates, then returns, the string that we've collected as part of the OSC string.
// Arguments:
// - string - Osc String input
// - title - Where to place the Osc String to use as a title.
// Return Value:
// - True if there was a title to output. (a title with length=0 is still valid)
bool OutputStateMachineEngine::_GetOscTitle(const std::wstring_view string,
                                            std::wstring& title) const
{
    title = string;

    return !string.empty();
}

// Routine Description:
// - Retrieves a distance for a tab operation from the parameter pool stored during Param actions.
// Arguments:
// - parameters - The parameters to parse
// - distance - Receives the distance
// Return Value:
// - True if we successfully pulled the tab distance from the parameters we've stored. False otherwise.
bool OutputStateMachineEngine::_GetTabDistance(const gsl::span<const size_t> parameters,
                                               size_t& distance) const noexcept
{
    bool success = false;
    distance = DefaultTabDistance;

    if (parameters.empty())
    {
        // Empty parameter sequences should use the default
        success = true;
    }
    else if (parameters.size() == 1)
    {
        // If there's one parameter, use it.
        distance = til::at(parameters, 0);
        success = true;
    }

    // Distances of 0 should be changed to 1.
    if (distance == 0)
    {
        distance = DefaultTabDistance;
    }

    return success;
}

// Routine Description:
// - Retrieves the type of tab clearing operation from the parameter pool stored during Param actions.
// Arguments:
// - parameters - The parameters to parse
// - clearType - Receives the clear type
// Return Value:
// - True if we successfully pulled the tab clear type from the parameters we've stored. False otherwise.
bool OutputStateMachineEngine::_GetTabClearType(const gsl::span<const size_t> parameters,
                                                size_t& clearType) const noexcept
{
    bool success = false;
    clearType = DefaultTabClearType;

    if (parameters.empty())
    {
        // Empty parameter sequences should use the default
        success = true;
    }
    else if (parameters.size() == 1)
    {
        // If there's one parameter, use it.
        clearType = til::at(parameters, 0);
        success = true;
    }
    return success;
}

// Method Description:
// - Returns true if the engine should attempt to parse a control sequence
//      following an SS3 escape prefix.
//   If this is false, an SS3 escape sequence should be dispatched as soon
//      as it is encountered.
// Return Value:
// - True iff we should parse a control sequence following an SS3.
bool OutputStateMachineEngine::ParseControlSequenceAfterSs3() const noexcept
{
    return false;
}

// Routine Description:
// - Returns true if the engine should dispatch on the last character of a string
//      always, even if the sequence hasn't normally dispatched.
//   If this is false, the engine will persist its state across calls to
//      ProcessString, and dispatch only at the end of the sequence.
// Return Value:
// - True iff we should manually dispatch on the last character of a string.
bool OutputStateMachineEngine::FlushAtEndOfString() const noexcept
{
    return false;
}

// Routine Description:
// - Returns true if the engine should dispatch control characters in the Escape
//      state. Typically, control characters are immediately executed in the
//      Escape state without returning to ground. If this returns true, the
//      state machine will instead call ActionExecuteFromEscape and then enter
//      the Ground state when a control character is encountered in the escape
//      state.
// Return Value:
// - True iff we should return to the Ground state when the state machine
//      encounters a Control (C0) character in the Escape state.
bool OutputStateMachineEngine::DispatchControlCharsFromEscape() const noexcept
{
    return false;
}

// Routine Description:
// - Returns false if the engine wants to be able to collect intermediate
//   characters in the Escape state. We do want to buffer characters as
//   intermediates. We need them for things like Designate G0 Character Set
// Return Value:
// - True iff we should dispatch in the Escape state when we encounter a
//   Intermediate character.
bool OutputStateMachineEngine::DispatchIntermediatesFromEscape() const noexcept
{
    return false;
}

// Routine Description:
// - Converts a hex character to its equivalent integer value.
// Arguments:
// - wch - Character to convert.
// - value - receives the int value of the char
// Return Value:
// - true iff the character is a hex character.
bool OutputStateMachineEngine::s_HexToUint(const wchar_t wch,
                                           unsigned int& value) noexcept
{
    value = 0;
    bool success = false;
    if (wch >= L'0' && wch <= L'9')
    {
        value = wch - L'0';
        success = true;
    }
    else if (wch >= L'A' && wch <= L'F')
    {
        value = (wch - L'A') + 10;
        success = true;
    }
    else if (wch >= L'a' && wch <= L'f')
    {
        value = (wch - L'a') + 10;
        success = true;
    }
    return success;
}

#pragma warning(push)
#pragma warning(disable : 26497) // We don't use any of these "constexprable" functions in that fashion

// Routine Description:
// - Determines if a character is a valid number character, 0-9.
// Arguments:
// - wch - Character to check.
// Return Value:
// - True if it is. False if it isn't.
static constexpr bool _isNumber(const wchar_t wch) noexcept
{
    return wch >= L'0' && wch <= L'9'; // 0x30 - 0x39
}

// Routine Description:
// - Determines if a character is a valid hex character, 0-9a-fA-F.
// Arguments:
// - wch - Character to check.
// Return Value:
// - True if it is. False if it isn't.
static constexpr bool _isHexNumber(const wchar_t wch) noexcept
{
    return (wch >= L'0' && wch <= L'9') || // 0x30 - 0x39
           (wch >= L'A' && wch <= L'F') ||
           (wch >= L'a' && wch <= L'f');
}

#pragma warning(pop)

// Routine Description:
// - Given a color spec string, attempts to parse the color that's encoded.
//   The only supported spec currently is the following:
//      spec: a color in the following format:
//          "rgb:<red>/<green>/<blue>"
//          where <color> is one or two hex digits, upper or lower case.
// Arguments:
// - string - The string containing the color spec string to parse.
// - rgb - receives the color that we parsed
// Return Value:
// - True if a color was successfully parsed
bool OutputStateMachineEngine::s_ParseColorSpec(const std::wstring_view string,
                                                DWORD& rgb) noexcept
{
    bool foundRGB = false;
    bool foundValidColorSpec = false;
    std::array<unsigned int, 3> colorValues = { 0 };
    bool success = false;
    // We can have anywhere between [11,15] characters
    // 9 "rgb:h/h/h"
    // 12 "rgb:hh/hh/hh"
    // Any fewer cannot be valid, and any more will be too many.
    // Return early in this case.
    //      We'll still have to bounds check when parsing the hh/hh/hh values
    if (string.size() < 9 || string.size() > 12)
    {
        return false;
    }

    // Now we look for "rgb:"
    // Other colorspaces are theoretically possible, but we don't support them.

    auto curr = string.cbegin();
    if ((*curr++ == L'r') &&
        (*curr++ == L'g') &&
        (*curr++ == L'b') &&
        (*curr++ == L':'))
    {
        foundRGB = true;
    }

    if (foundRGB)
    {
        // Colorspecs are up to hh/hh/hh, for 1-2 h's
        for (size_t component = 0; component < 3; component++)
        {
            bool foundColor = false;
            auto& value = til::at(colorValues, component);
            for (size_t i = 0; i < 3; i++)
            {
                const wchar_t wch = *curr++;

                if (_isHexNumber(wch))
                {
                    value *= 16;
                    unsigned int intVal = 0;
                    if (s_HexToUint(wch, intVal))
                    {
                        value += intVal;
                    }
                    else
                    {
                        // Encountered something weird oh no
                        foundColor = false;
                        break;
                    }
                    // If we're on the blue component, we're not going to see a /.
                    // Break out once we hit the end.
                    if (component == 2 && curr >= string.cend())
                    {
                        foundValidColorSpec = true;
                        break;
                    }
                }
                else if (wch == L'/')
                {
                    // Break this component, and start the next one.
                    foundColor = true;
                    break;
                }
                else
                {
                    // Encountered something weird oh no
                    foundColor = false;
                    break;
                }
            }
            if (!foundColor || curr >= string.cend())
            {
                // Indicates there was a some error parsing color
                //  or we're at the end of the string.
                break;
            }
        }
    }
    // Only if we find a valid colorspec can we pass it out successfully.
    if (foundValidColorSpec)
    {
        DWORD color = RGB(LOBYTE(colorValues.at(0)),
                          LOBYTE(colorValues.at(1)),
                          LOBYTE(colorValues.at(2)));

        rgb = color;
        success = true;
    }
    return success;
}

// Routine Description:
// - OSC 4 ; c ; spec ST
//      c: the index of the ansi color table
//      spec: a color in the following format:
//          "rgb:<red>/<green>/<blue>"
//          where <color> is two hex digits
// Arguments:
// - string - the Osc String to parse
// - tableIndex - receives the table index
// - rgb - receives the color that we parsed in the format: 0x00BBGGRR
// Return Value:
// - True if a table index and color was parsed successfully. False otherwise.
bool OutputStateMachineEngine::_GetOscSetColorTable(const std::wstring_view string,
                                                    size_t& tableIndex,
                                                    DWORD& rgb) const noexcept
{
    tableIndex = 0;
    rgb = 0;
    size_t _TableIndex = 0;

    bool foundTableIndex = false;
    bool success = false;
    // We can have anywhere between [11,16] characters
    // 11 "#;rgb:h/h/h"
    // 16 "###;rgb:hh/hh/hh"
    // Any fewer cannot be valid, and any more will be too many.
    // Return early in this case.
    //      We'll still have to bounds check when parsing the hh/hh/hh values
    if (string.size() < 11 || string.size() > 16)
    {
        return false;
    }

    // First try to get the table index, a number between [0,256]
    size_t current = 0;
    for (size_t i = 0; i < 4; i++)
    {
        const wchar_t wch = string.at(current);
        if (_isNumber(wch))
        {
            _TableIndex *= 10;
            _TableIndex += wch - L'0';

            ++current;
        }
        else if (wch == L';' && i > 0)
        {
            // We need to explicitly pass in a number, we can't default to 0 if
            //  there's no param
            ++current;
            foundTableIndex = true;
            break;
        }
        else
        {
            // Found an unexpected character, fail.
            break;
        }
    }
    // Now we look for "rgb:"
    // Other colorspaces are theoretically possible, but we don't support them.
    if (foundTableIndex)
    {
        DWORD color = 0;
        success = s_ParseColorSpec(string.substr(current), color);

        if (success)
        {
            tableIndex = _TableIndex;
            rgb = color;
        }
    }

    return success;
}

// Routine Description:
// - Given a hyperlink string, attempts to parse the URI encoded. An 'id' parameter
//   may be provided.
//   If there is a URI, the well formatted string looks like:
//          "<params>;<URI>"
//   If there is no URI, we need to close the hyperlink and the string looks like:
//          ";"
// Arguments:
// - string - the string containing the parameters and URI
// - params - where to store the parameters
// - uri - where to store the uri
// Return Value:
// - True if a URI was successfully parsed or if we are meant to close a hyperlink
bool OutputStateMachineEngine::_ParseHyperlink(const std::wstring_view string,
                                               std::wstring& params,
                                               std::wstring& uri) const
{
    params.clear();
    uri.clear();
    const auto len = string.size();
    const size_t midPos = string.find(';');
    if (midPos != std::wstring::npos)
    {
        if (len != 1)
        {
            uri = string.substr(midPos + 1);
            const auto paramStr = string.substr(0, midPos);
            const auto idPos = paramStr.find(hyperlinkIDParameter);
            if (idPos != std::wstring::npos)
            {
                params = paramStr.substr(idPos + hyperlinkIDParameter.size());
            }
        }
        return true;
    }
    return false;
}

// Routine Description:
// - OSC 10, 11, 12 ; spec ST
//      spec: a color in the following format:
//          "rgb:<red>/<green>/<blue>"
//          where <color> is two hex digits
// Arguments:
// - string - the Osc String to parse
// - rgb - receives the color that we parsed in the format: 0x00BBGGRR
// Return Value:
// - True if a table index and color was parsed successfully. False otherwise.
bool OutputStateMachineEngine::_GetOscSetColor(const std::wstring_view string,
                                               DWORD& rgb) const noexcept
{
    rgb = 0;

    bool success = false;

    DWORD color = 0;
    success = s_ParseColorSpec(string, color);

    if (success)
    {
        rgb = color;
    }

    return success;
}

// Method Description:
// - Retrieves the type of window manipulation operation from the parameter pool
//      stored during Param actions.
//  This is kept separate from the input version, as there may be
//      codes that are supported in one direction but not the other.
// Arguments:
// - parameters - The parameters to parse
// - function - Receives the function type
// Return Value:
// - True iff we successfully pulled the function type from the parameters
bool OutputStateMachineEngine::_GetWindowManipulationType(const gsl::span<const size_t> parameters,
                                                          unsigned int& function) const noexcept
{
    bool success = false;
    function = DefaultWindowManipulationType;

    if (parameters.size() > 0)
    {
        switch (til::at(parameters, 0))
        {
        case DispatchTypes::WindowManipulationType::RefreshWindow:
            function = DispatchTypes::WindowManipulationType::RefreshWindow;
            success = true;
            break;
        case DispatchTypes::WindowManipulationType::ResizeWindowInCharacters:
            function = DispatchTypes::WindowManipulationType::ResizeWindowInCharacters;
            success = true;
            break;
        default:
            success = false;
            break;
        }
    }

    return success;
}

// Routine Description:
// - Retrieves the cursor style from the parameter list
// Arguments:
// - parameters - The parameters to parse
// - cursorStyle - Receives the cursorStyle
// Return Value:
// - True if we successfully pulled the cursor style from the parameters we've stored. False otherwise.
bool OutputStateMachineEngine::_GetCursorStyle(const gsl::span<const size_t> parameters,
                                               DispatchTypes::CursorStyle& cursorStyle) const noexcept
{
    bool success = false;
    cursorStyle = DefaultCursorStyle;

    if (parameters.empty())
    {
        // Empty parameter sequences should use the default
        success = true;
    }
    else if (parameters.size() == 1)
    {
        // If there's one parameter, use it.
        cursorStyle = (DispatchTypes::CursorStyle)til::at(parameters, 0);
        success = true;
    }

    return success;
}

// Method Description:
// - Sets us up to have another terminal acting as the tty instead of conhost.
//      We'll set a couple members, and if they aren't null, when we get a
//      sequence we don't understand, we'll pass it along to the terminal
//      instead of eating it ourselves.
// Arguments:
// - pTtyConnection: This is a TerminalOutputConnection that we can write the
//      sequence we didn't understand to.
// - pfnFlushToTerminal: This is a callback to the underlying state machine to
//      trigger it to call ActionPassThroughString with whatever sequence it's
//      currently processing.
// Return Value:
// - <none>
void OutputStateMachineEngine::SetTerminalConnection(ITerminalOutputConnection* const pTtyConnection,
                                                     std::function<bool()> pfnFlushToTerminal)
{
    this->_pTtyConnection = pTtyConnection;
    this->_pfnFlushToTerminal = pfnFlushToTerminal;
}

// Routine Description:
// - Retrieves a number of times to repeat the last graphical character
// Arguments:
// - parameters - The parameters to parse
// - repeatCount - Receives the repeat count
// Return Value:
// - True if we successfully pulled the repeat count from the parameters.
//   False otherwise.
bool OutputStateMachineEngine::_GetRepeatCount(gsl::span<const size_t> parameters,
                                               size_t& repeatCount) const noexcept
{
    bool success = false;
    repeatCount = DefaultRepeatCount;

    if (parameters.empty())
    {
        // Empty parameter sequences should use the default
        success = true;
    }
    else if (parameters.size() == 1)
    {
        // If there's one parameter, use it.
        repeatCount = til::at(parameters, 0);
        success = true;
    }

    // Distances of 0 should be changed to 1.
    if (repeatCount == 0)
    {
        repeatCount = DefaultRepeatCount;
    }

    return success;
}

// Routine Description:
// - Parse OscSetClipboard parameters with the format `Pc;Pd`. Currently the first parameter `Pc` is
// ignored. The second parameter `Pd` should be a valid base64 string or character `?`.
// Arguments:
// - string - Osc String input.
// - content - Content to set to clipboard.
// - queryClipboard - Whether to get clipboard content and return it to terminal with base64 encoded.
// Return Value:
// - True if there was a valid base64 string or the passed parameter was `?`.
bool OutputStateMachineEngine::_GetOscSetClipboard(const std::wstring_view string,
                                                   std::wstring& content,
                                                   bool& queryClipboard) const noexcept
{
    const size_t pos = string.find(';');
    if (pos != std::wstring_view::npos)
    {
        const std::wstring_view substr = string.substr(pos + 1);
        if (substr == L"?")
        {
            queryClipboard = true;
            return true;
        }
        else
        {
            return Base64::s_Decode(substr, content);
        }
    }

    return false;
}

// Method Description:
// - Clears our last stored character. The last stored character is the last
//      graphical character we printed, which is reset if any other action is
//      dispatched.
// Arguments:
// - <none>
// Return Value:
// - <none>
void OutputStateMachineEngine::_ClearLastChar() noexcept
{
    _lastPrintedChar = AsciiChars::NUL;
}
