#include <gtest/gtest.h>

#include <quickfix/Exceptions.h>
#include <quickfix/Message.h>

#include "message_translator.hpp"

using namespace tse::fix;

// These exercise the parsing boundary directly: constructing a FIX::Message
// from raw, deliberately malformed text. In a live session this same
// exception path is what QuickFIX's own socket/Session layer catches
// internally before a malformed message would ever reach our Application —
// these tests prove that boundary is exception-safe (throws cleanly, never
// crashes), which is the actual mechanism behind "malformed messages don't
// crash the session."

TEST(MalformedMessage, EmptyStringThrowsInvalidMessage) {
    EXPECT_THROW(FIX::Message(std::string(""), true), FIX::InvalidMessage);
}

TEST(MalformedMessage, CompletelyNonFixTextThrowsInvalidMessage) {
    EXPECT_THROW(FIX::Message(std::string("this is not a fix message at all, just garbage bytes"), true),
                 FIX::InvalidMessage);
}

TEST(MalformedMessage, TruncatedMessageThrowsInvalidMessage) {
    // A NewOrderSingle header cut off mid-body — no trailing CheckSum(10=).
    std::string truncated = "8=FIX.4.2\x01"
                             "9=100\x01"
                             "35=D\x01"
                             "49=SIM\x01"
                             "56=EXCH\x01"
                             "34=1\x01";
    EXPECT_THROW(FIX::Message(truncated, true), FIX::InvalidMessage);
}

TEST(MalformedMessage, CorruptedChecksumThrowsInvalidMessage) {
    Order order;
    order.order_id = "ORD-1";
    order.account_id = "ACC-1";
    order.instrument_id = "ACME";
    order.side = Side::kBuy;
    order.price = 100.0;
    order.qty = 100;
    order.timestamp_ns = 0;
    order.status = OrderStatus::kNew;
    order.venue = "SIM";

    std::string wire = to_new_order_single(order).toString();
    ASSERT_GE(wire.size(), 4u);
    // Flip the checksum's tens digit so 10=xyz no longer matches the
    // computed sum.
    size_t checksum_digit_pos = wire.size() - 2;
    wire[checksum_digit_pos] = (wire[checksum_digit_pos] == '0') ? '1' : '0';

    EXPECT_THROW(FIX::Message(wire, true), FIX::InvalidMessage);
}

TEST(MalformedMessage, GarbageBytesInsteadOfSohDelimitersThrowsInvalidMessage) {
    // Same tag=value content as a real message but with '|' instead of the
    // FIX SOH (0x01) delimiter — structurally unparseable as FIX.
    std::string mangled = "8=FIX.4.2|9=50|35=D|49=SIM|56=EXCH|34=1|52=x|10=000|";
    EXPECT_THROW(FIX::Message(mangled, true), FIX::InvalidMessage);
}

TEST(MalformedMessage, MissingRequiredFieldThrowsFieldNotFound) {
    // Syntactically constructible, but ClOrdID(11) — required on
    // NewOrderSingle — is deliberately never set. This is what
    // fix_application.cpp's onMessage() relies on: message_translator
    // throwing FIX::FieldNotFound here is exactly the exception
    // FIX::Application::fromApp's contract expects, and Session::next()
    // catches it internally rather than letting it crash the process.
    FIX42::NewOrderSingle message;
    message.set(FIX::Symbol("ACME"));
    message.set(FIX::Side(FIX::Side_BUY));

    EXPECT_THROW({ Order unused = from_new_order_single(message); }, FIX::FieldNotFound);
}
