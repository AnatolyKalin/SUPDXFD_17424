// Copyright (c) 2024 Devexperts LLC.
// SPDX-License-Identifier: MPL-2.0

#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include <DXErrorCodes.h>
#include <DXFeed.h>

#ifdef _MSC_FULL_VER
#pragma warning(push)
#pragma warning(disable : 4244)
#endif

struct StringConverter {
    static std::string toString(const std::wstring &wstring) {
        return std::string(wstring.begin(), wstring.end());
    }

    static std::string toString(wchar_t wchar) {
        return toString(std::wstring(1, wchar));
    }

    template<typename InputIterator>
    static std::string toString(InputIterator first, InputIterator last) {
        return toString(std::wstring(first, last));
    }

    static std::wstring toWString(const std::string &string) {
        return std::wstring(string.begin(), string.end());
    }
};

#ifdef _MSC_FULL_VER
#pragma warning(pop)
#endif

enum TimeZone { LOCAL,
                GMT };

template<TimeZone>
inline std::string formatTime(long long timestamp, const std::string &format = "%Y-%m-%d %H:%M:%S");

template<>
inline std::string formatTime<LOCAL>(long long timestamp, const std::string &format) {
    return fmt::format(fmt::format("{{:{}}}", format), fmt::localtime(static_cast<std::time_t>(timestamp)));
}

template<>
inline std::string formatTime<GMT>(long long timestamp, const std::string &format) {
    return fmt::format(fmt::format("{{:{}}}", format), fmt::gmtime(static_cast<std::time_t>(timestamp)));
}

template<TimeZone tz>
inline std::string formatTimestampWithMillis(long long timestamp) {
    long long ms = timestamp % 1000;

    return fmt::format("{}.{:0>3}", formatTime<tz>(timestamp / 1000), ms);
}

#define UNIQUE_NAME_LINE2(name, line) name##line
#define UNIQUE_NAME_LINE(name, line) UNIQUE_NAME_LINE2(name, line)
#define UNIQUE_NAME(name) UNIQUE_NAME_LINE(name, __LINE__)

namespace detail {
    template<typename F>
    constexpr auto onScopeExitImpl(F &&f) {
        auto onExit = [&f](auto) { f(); };

        return std::shared_ptr<void>(nullptr, onExit);
    }
}// namespace detail

#define onScopeExit(...) auto UNIQUE_NAME(FINALLY_) = detail::onScopeExitImpl(__VA_ARGS__)

inline void printTimestamp(dxf_long_t timestamp, dxf_const_string_t keyName = L"") {
    if (keyName && keyName[0] != 0) {
        std::wcout << keyName << " = ";
    }

    std::wcout << StringConverter::toWString(formatTimestampWithMillis<LOCAL>(timestamp)).c_str();
}

inline dxf_const_string_t orderScopeToString(dxf_order_scope_t scope) {
    switch (scope) {
        case dxf_osc_composite:
            return L"Composite";
        case dxf_osc_regional:
            return L"Regional";
        case dxf_osc_aggregate:
            return L"Aggregate";
        case dxf_osc_order:
            return L"Order";
    }

    return L"";
}

inline dxf_const_string_t orderSideToString(dxf_order_side_t side) {
    switch (side) {
        case dxf_osd_undefined:
            return L"Undefined";
        case dxf_osd_buy:
            return L"Buy";
        case dxf_osd_sell:
            return L"Sell";
    }

    return L"";
}

std::recursive_mutex ioMutex{};

inline void processLastError() {
    std::lock_guard<std::recursive_mutex> lock{ioMutex};

    int errorCode = dx_ec_success;

    dxf_const_string_t errorDescription = nullptr;
    auto res = dxf_get_last_error(&errorCode, &errorDescription);

    if (res == DXF_SUCCESS) {
        if (errorCode == dx_ec_success) {
            std::wcout << L"No error information is stored" << std::endl;

            return;
        }

        std::wcout << L"Error occurred and successfully retrieved:\nerror code = " << errorCode << ", description = \""
                   << errorDescription << "\"" << std::endl;

        return;
    }

    std::wcout << L"An error occurred but the error subsystem failed to initialize" << std::endl;
}

using ListenerType = void(int /*eventType*/, dxf_const_string_t /*symbolName*/, const dxf_event_data_t * /*data*/,
                          int /*dataCount*/, void * /*userData*/);
using ListenerPtrType = std::add_pointer_t<ListenerType>;

struct SubscriptionBase {
    virtual ~SubscriptionBase() = default;
    virtual void Close() = 0;
};

template <typename F, typename... Args>
void log(F&& format, Args&&... args) {
    std::lock_guard<std::recursive_mutex> lock{ioMutex};
    fmt::print(format, args...);
}

template<std::size_t id>
struct Subscription : public SubscriptionBase {
    std::recursive_mutex mutex{};
    dxf_connection_t connection{nullptr};
    dxf_const_string_t symbol{nullptr};
    dxf_subscription_t handle{nullptr};
    ERRORCODE errorCode{DXF_SUCCESS};

    Subscription(dxf_connection_t connection, dxf_const_string_t symbol) : connection(connection), symbol(symbol) {
        log("Sub[id = {}]: Creating a subscription\n", id);

        errorCode = dxf_create_subscription(connection, DXF_ET_QUOTE, &handle);

        if (errorCode == DXF_FAILURE) {
            processLastError();

            return;
        }

        log("Sub[id = {}, handle = {}]: Attaching the listener: {}\n", id, (void*)handle, (void *) getListener());

        errorCode = dxf_attach_event_listener(handle, getListener(), (void *) (std::size_t{id}));

        if (errorCode == DXF_FAILURE) {
            processLastError();

            return;
        }

        log("Sub[id = {}, handle = {}]: Adding the symbol: {}\n", id, (void*)handle, StringConverter::toString(symbol));

        errorCode = dxf_add_symbol(handle, symbol);

        if (errorCode == DXF_FAILURE) {
            processLastError();
        }
    }

    static inline ListenerPtrType getListener() {
        static ListenerPtrType l = [](int eventType, dxf_const_string_t symbolName, const dxf_event_data_t *data,
                                      int dataCount, void *userData) {
            std::lock_guard<std::recursive_mutex> lock{ioMutex};

            std::wcout << "Sub[" << id << "]: Listener[" << (std::size_t) userData << "]: ";

            if (eventType == DXF_ET_QUOTE) {
                auto *q = (dxf_quote_t *) data;

                std::wcout << L"Quote{symbol = " << symbolName;
                std::wcout << L" bidTime = ";
                printTimestamp(q->bid_time);
                std::wcout << L" bidExchangeCode = " << q->bid_exchange_code << ", bidPrice = " << q->bid_price << ", bidSize=" << q->bid_size << ", ";
                std::wcout << L"askTime = ";
                printTimestamp(q->ask_time);
                std::wcout << L" askExchangeCode = " << q->ask_exchange_code << ", askPrice = " << q->bid_price << ", askSize=" << q->bid_size << ", ";
                std::wcout << L"scope = " << orderScopeToString(q->scope) << "}" << std::endl;
            }
        };

        return l;
    }

    void CloseImpl() {
        if (handle && errorCode == DXF_SUCCESS) {
            log("Sub[id = {}, handle = {}]: Removing the symbol: {}\n", id, (void*)handle, StringConverter::toString(symbol));

            errorCode = dxf_remove_symbol(handle, symbol);

            if (errorCode == DXF_FAILURE) {
                processLastError();

                return;
            }

            log("Sub[id = {}, handle = {}]: Detaching the listener: {}\n", id, (void*)handle, (void *) getListener());

            auto result = dxf_detach_event_listener(handle, getListener());

            if (result == DXF_FAILURE) {
                processLastError();

                return;
            }

            log("Sub[id = {}, handle = {}]: Closing the subscription\n", id, (void*)handle);

            errorCode = dxf_close_subscription(handle);

            if (errorCode == DXF_FAILURE) {
                processLastError();
            }

            handle = nullptr;
        }
    }

    void Close() override {
        std::lock_guard<std::recursive_mutex> lock{mutex};
        CloseImpl();
    }

    ~Subscription() override {
        CloseImpl();
    }
};


int main() {
    dxf_initialize_logger_v2("SUPDXFD-17424.log", true, true, true, false);
    dxf_load_config_from_string("logger.level = \"debug\"\n");
    auto symbol = L"ETH/USD";

    dxf_connection_t c{};

    auto result = dxf_create_connection("demo.dxfeed.com:7300", nullptr, nullptr, nullptr, nullptr, nullptr, &c);

    if (result == DXF_FAILURE) {
        processLastError();

        return 1;
    }

    onScopeExit([&c] {
        auto result = dxf_close_connection(c);

        if (result == DXF_FAILURE) {
            processLastError();
        }
    });

    std::vector<std::unique_ptr<SubscriptionBase>> subs{};

    subs.emplace_back(new Subscription<1>(c, symbol));

    std::this_thread::sleep_for(std::chrono::seconds(3));

    subs.emplace_back(new Subscription<2>(c, symbol));

    std::this_thread::sleep_for(std::chrono::seconds(3));

    subs.emplace_back(new Subscription<3>(c, symbol));

    std::this_thread::sleep_for(std::chrono::seconds(3));

    subs.emplace_back(new Subscription<4>(c, symbol));

    std::this_thread::sleep_for(std::chrono::seconds(3));

    subs.emplace_back(new Subscription<5>(c, symbol));

    std::this_thread::sleep_for(std::chrono::seconds(3));

    subs[2]->Close();

    std::this_thread::sleep_for(std::chrono::seconds(3));

    return 0;
}
