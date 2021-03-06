#include "stdafx.h"
#include <cstdint>
#include <map>
#include <vector>
#include <string>
#include <iterator>
#include <assert.h>
#include <algorithm>
#include <regex>
#include <locale>
#include <Ws2tcpip.h>
#include <memory>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <comutil.h>

struct RequestInfo {
	RequestInfo(SOCKET sock_, const SOCKADDR_STORAGE& addr_, std::uint16_t id_) 
		: sock{ sock_ }
		, addr {addr_}
		, id{ id_ }
	{}
	SOCKET sock;
	SOCKADDR_STORAGE addr;
	std::uint16_t id;
};
std::uint16_t nextId;
std::map<std::uint16_t, RequestInfo> requests;

class Config {
	std::vector<std::string> m_accepted_domains;
public:
	Config(const std::wstring& filename)
	{
		std::ifstream in; { };
		in.open(filename.c_str(), std::ios_base::in);
		while (in) {
			char buffer[512];
			in.getline(buffer, 512);
			if (strnlen_s(buffer, 512) > 0)
				m_accepted_domains.emplace_back(buffer);
		}
	}
	const std::vector<std::string>& accepted_domains() const {
		return m_accepted_domains;
	}
};
std::shared_ptr<Config> config;

std::wstring ToWstring(const char* s)
{
	return (const wchar_t *)_bstr_t { s };
}
bool icompare_pred(char a, char b)
{
	auto&& ctype = std::use_facet<std::ctype<char>>(std::locale::classic());
	return ctype.tolower(a) == ctype.tolower(b);
}

bool accept(const std::string& domain, const std::string& mask)
{
	if (domain.length() < mask.length())
		return false;
	size_t offset = domain.length() - mask.length();
	if (offset > 0 && domain[offset - 1] != '.')
		return false;
	bool ret = std::equal(domain.begin()+offset, domain.end(), mask.begin(), mask.end(), icompare_pred);
	return ret;
}

bool Accept(const std::string& name)
{
	auto ptr = config;
	bool ret = std::any_of(ptr->accepted_domains().begin(), ptr->accepted_domains().end(),
		[&name](const auto& mask) { return accept(name, mask); });
	return ret;
}

const wchar_t* StringError(DWORD error)
{
	static wchar_t buffer[1024];
	DWORD err = FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM| FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0x0409, buffer, _countof(buffer), nullptr);
	if (err == 0)
		err = GetLastError();
	return buffer;
}


class AsyncOp : protected OVERLAPPED {
public:
	static HANDLE hExit;
	static SOCKET outbound;
	static SOCKET sock4;
	static SOCKET sock6;
	AsyncOp(SOCKET inbound_)
		: inbound{ inbound_ }
	{
		SecureZeroMemory(this, sizeof(OVERLAPPED));
		hEvent = WSACreateEvent();
		buf.len = DNS_RFC_MAX_UDP_PACKET_LENGTH;
		buf.buf = new char[DNS_RFC_MAX_UDP_PACKET_LENGTH];
	}
	~AsyncOp()
	{
		delete[] buf.buf;
		WSACloseEvent(hEvent);
	}
	static bool Init(const wchar_t* real_dns_ip)
	{
		WSADATA wsaData;
		int err = WSAStartup(MAKEWORD(2, 2), &wsaData);
		if (err) {
			wprintf_s(L"Socket library startup failed %s\n", StringError(err));
			return false;
		}
		sock4 = WSASocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED);
		{
			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_port = htons(53);
			int ret = InetPton(AF_INET, L"0.0.0.0", &addr.sin_addr);
			if (ret != 1) {
				wprintf_s(L"Bad IPv4 address\n");
				return false;
			}
			if (SOCKET_ERROR == bind(sock4, (SOCKADDR*)&addr, sizeof(addr))) {
				wprintf_s(L"Opening listening socket failed %s\n", StringError(WSAGetLastError()));
				return false;
			}
		}
		sock6 = WSASocket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED);
		{
			sockaddr_in6 addr{};
			addr.sin6_family = AF_INET6;
			addr.sin6_port = htons(53);
			int ret = InetPton(AF_INET6, L"::0", &addr.sin6_addr);
			if (ret != 1) {
				wprintf_s(L"Bad IPv6 address\n");
				return false;
			}
			if (SOCKET_ERROR == bind(sock6, (SOCKADDR*)&addr, sizeof(addr))) {
				wprintf_s(L"Opening listening socket failed %s\n", StringError(WSAGetLastError()));
				return false;
			}
		}
		outbound = WSASocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED);
		{
			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_port = 0;
			if (SOCKET_ERROR == bind(outbound, (SOCKADDR*)&addr, sizeof(addr))) {
				wprintf_s(L"Opening listening socket failed %s\n", StringError(WSAGetLastError()));
				return false;
			}
		}
		{
			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_port = htons(53);
			if (InetPton(AF_INET, real_dns_ip, &addr.sin_addr) <= 0) {
				wprintf_s(L"Bad DNS IP provided %s\n", real_dns_ip);
				return false;
			}
			memcpy(&dns_addr, &addr, sizeof(addr));
		}
		hExit = CreateEvent(NULL, TRUE, FALSE, NULL);
		SetConsoleCtrlHandler(CtrlBreak, TRUE);
		return true;
	}
	static void Shutdown()
	{
		CancelIo((HANDLE)AsyncOp::sock4);
		CancelIo((HANDLE)AsyncOp::sock6);
		CancelIo((HANDLE)AsyncOp::outbound);
		SleepEx(1000, TRUE);
		CloseHandle(hExit);
		closesocket(sock4);
		closesocket(sock6);
		closesocket(outbound);
		WSACleanup();
	}
protected:
	WSABUF buf;
	SOCKADDR_STORAGE addr;
	SOCKET inbound;
	static SOCKADDR_STORAGE dns_addr;
	static BOOL WINAPI CtrlBreak(DWORD)
	{
		Stop();
		return TRUE;
	}
	static void Stop()
	{
		SetEvent(hExit);
	}
};
SOCKET AsyncOp::sock4;
SOCKET AsyncOp::sock6;
SOCKET AsyncOp::outbound;
HANDLE AsyncOp::hExit;
SOCKADDR_STORAGE AsyncOp::dns_addr;

class NameParser {
	PDNS_HEADER hdr;
	std::uint16_t counter;
	char *base;
	std::string item;
public:
	typedef std::string value_type;
	typedef std::string& reference;
	typedef std::string* pointer;
	typedef intptr_t difference_type;
	typedef std::forward_iterator_tag iterator_category;
	NameParser(PDNS_HEADER hdr_)
		: hdr{ hdr_ }
		, counter{ hdr->QuestionCount }
		, base{ DNS_QUESTION_NAME_FROM_HEADER(hdr)}
	{
		item.reserve(DNS_MAX_NAME_LENGTH);
		++counter;
		operator++();
	}
	NameParser()
		: hdr{ nullptr }
		, counter{ 0 }
	{}
	bool operator==(const NameParser& it) const
	{
		return counter == it.counter;
	}
	bool operator!=(const NameParser& it) const
	{
		return !operator==(it);
	}
	std::string& operator*() {
		return item;
	}
	const std::string& operator*() const { return item; }
	const std::string* operator->() const { return &item; }
	NameParser& operator++() {
		assert(counter > 0);
		item.clear();
		--counter;
		if (counter > 0)
			base = parse(base);
		return *this;
	}
	NameParser operator++(int) {
		NameParser it = *this;
		operator++();
		return it;
	}
private:
	inline std::uint16_t read16(char *p)
	{
		return std::uint16_t(*p) << 8 | *(p + 1);
	}
	char* parse(char* p)
	{
		while (*p != 0) {
			if (*p & 0xC0) {
				std::uint16_t len = read16(p);
				len &= 0x3F;
				p = (char*)base + len;
				parse((char*)base + len);
				return p + 2;
			}
			size_t len = *p++;
			if (!item.empty())
				item.append(".");
			item.append(p, len);
			p += len;
		}
		return p + 1;
	}
};

class DnsAsyncOp : public AsyncOp {
	int addr_len;
	DWORD flags;
public:
	DnsAsyncOp(SOCKET s) : AsyncOp(s)
	{
		Recv();
	}
	void Recv()
	{
		flags = 0;
		addr_len = sizeof(addr);
		for (int attempt = 0; attempt < 10; ++attempt) {
			buf.len = DNS_RFC_MAX_UDP_PACKET_LENGTH;
			if (!WSARecvFrom(inbound, &buf, 1, nullptr, &flags, (SOCKADDR*)&addr, &addr_len, this, RecvCompletionS)
				|| CheckResult(WSAGetLastError(), L"WSARecvFrom %s failed with %s\n"))
				return;
		}
		Stop();
	}
	void Send(SOCKET s)
	{
		for (int attempt = 0; attempt < 10; ++attempt)
			if (!WSASendTo(s, &buf, 1, nullptr, 0, (SOCKADDR*)&addr, sizeof(addr), this, SendCompletionS) 
				|| CheckResult(WSAGetLastError(), L"WSASendTo %s failed with %s\n"))
				return;
		Stop();
	}
	std::wstring AddrToStr()
	{
		wchar_t straddr[64];
		DWORD len = 64;
		WSAAddressToStringW((LPSOCKADDR)&addr, sizeof(addr), nullptr, straddr, &len);
		return straddr;
	}
	bool CheckResult(int ret, const wchar_t* message)
	{
		if (ret != 0 && ret != WSA_IO_PENDING) {
			wchar_t straddr[64];
			DWORD len = 64;
			WSAAddressToStringW((LPSOCKADDR)&addr, sizeof(addr), nullptr, straddr, &len);
			wprintf_s(message, straddr, StringError(ret) );
			return false;
		}
		return true;
	}
private:
	void RecvCompletion(DWORD dwError, DWORD cbRead)
	{
		if (!CheckResult(dwError, L"Recv from %s failed with %s\n") || cbRead < sizeof(DNS_HEADER)) {
			Recv();
			return;
		}
		buf.len = cbRead;
		PDNS_HEADER hdr = (PDNS_HEADER)buf.buf;
		DNS_BYTE_FLIP_HEADER_COUNTS(hdr);
		std::wostringstream stm;
		stm << L"Read " << (hdr->IsResponse ? L" response " : L" request ")
			<< cbRead << L" bytes from " << AddrToStr() 
			<< L" opcode = " << hdr->Opcode 
			<< L" response code = " << hdr->ResponseCode
			<< L" Xid = " << hdr->Xid;
		if (!hdr->IsResponse) {
			for (auto it = NameParser(hdr); it != NameParser(); ++it)
				stm << L" " << ToWstring(it->c_str());
			bool accept = hdr->Opcode != 0 || std::all_of(NameParser(hdr), NameParser(), Accept);
			SOCKET s;
			if (accept) {
				std::uint16_t id = ++nextId;
				requests.emplace(id, RequestInfo(inbound, addr, hdr->Xid));
				hdr->Xid = id;
				addr = dns_addr;
				s = outbound;
				stm << L" Xid->" << id << L" throw";
			}
			else {
				stm << L" deny";
				hdr->IsResponse = 0;
				hdr->ResponseCode = 5;
				s = inbound;
			}
			DNS_BYTE_FLIP_HEADER_COUNTS(hdr);
			Send(s);
		}
		else {
			auto it = requests.find(hdr->Xid);
			if (it != requests.end()) {
				hdr->Xid = it->second.id;
				addr = it->second.addr;
				DNS_BYTE_FLIP_HEADER_COUNTS(hdr);
				Send(it->second.sock);
				requests.erase(it);
			}
			else {
				stm << L" unknown response";
				Recv();
			}
		}
		stm << L"\n";
		wprintf_s(stm.str().c_str());
	}
	void SendCompletion(DWORD dwError, DWORD cbRead)
	{
		if (!CheckResult(dwError, L"Send to %s failed with %s\n")) {
			PDNS_HEADER hdr = (PDNS_HEADER)buf.buf;
			DNS_BYTE_FLIP_HEADER_COUNTS(hdr);
			if (!hdr->IsResponse)
				requests.erase(hdr->Xid);
		}
		Recv();
	}
	static void CALLBACK RecvCompletionS(DWORD dwError, DWORD cbTransferred, LPWSAOVERLAPPED lpOverlapped, DWORD dwFlags)
	{
		if (dwError == ERROR_OPERATION_ABORTED)
			return;
		static_cast<DnsAsyncOp*>(lpOverlapped)->RecvCompletion(dwError, cbTransferred);
	}
	static void CALLBACK SendCompletionS(DWORD dwError, DWORD cbTransferred, LPWSAOVERLAPPED lpOverlapped, DWORD dwFlags)
	{
		if (dwError == ERROR_OPERATION_ABORTED)
			return;
		static_cast<DnsAsyncOp*>(lpOverlapped)->SendCompletion(dwError, cbTransferred);
	}
};

int wmain(int argc, wchar_t *argv[])
{
	if (argc == 2 && wcscmp(argv[1], L"/?")==0) {
		wprintf_s(L"Usage:\nDnsFilter [real dns ip]\nIf real dns ip is not pointed the 8.8.8.8 used\n");
		return 0;
	}
	namespace fs = std::experimental::filesystem;
	fs::path p = argv[0];
	p.remove_filename();
	p.append(L"config.ini");
	if (!fs::exists(p))
	{
		wprintf_s(L"I don't see config at %s", p.c_str());
		return 1;
	}
	config = std::make_shared<Config>(p);
	if (!AsyncOp::Init(argc<2 ? L"8.8.8.8" : argv[1]))
		return 1;
	wprintf_s(L"DNS filter running...\nPress Ctrl+Break to terminate\n");
	HANDLE hConfig = FindFirstChangeNotification(fs::current_path().c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE);
	HANDLE handles[2] = { AsyncOp::hExit, hConfig };
	std::vector<std::unique_ptr<DnsAsyncOp>> executors;
	executors.emplace_back(new DnsAsyncOp(AsyncOp::sock4));
	executors.emplace_back(new DnsAsyncOp(AsyncOp::sock6));
	executors.emplace_back(new DnsAsyncOp(AsyncOp::outbound));
	for (;;) {
		auto ret = WaitForMultipleObjectsEx(2, handles, FALSE, INFINITE, TRUE);
		if (ret == WAIT_OBJECT_0 + 1) {
			config = std::make_shared<Config>(p);
			wprintf_s(L"Configuration has been updated\n");
			FindNextChangeNotification(hConfig);
		}
		else if (ret == WAIT_IO_COMPLETION)
			continue;
		else
			break;
	}
	FindCloseChangeNotification(hConfig);
	wprintf_s(L"Application is terminating...\n");
	AsyncOp::Shutdown();
    return 0;
}

