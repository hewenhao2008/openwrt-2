//////////////////////////////////////////////////////////////////
//
// Toolkit base class for the GnuGk
//
// This work is published under the GNU Public License (GPL)
// see file COPYING for details.
// We also explicitly grant the right to link this code
// with the OpenH323 library.
//
//////////////////////////////////////////////////////////////////

#include <ptlib.h>
#include <ptclib/pdns.h>
#include <ptclib/cypher.h>
#include <h323pdu.h>
#include <map>
#include "stl_supp.h"
#include "gktimer.h"
#include "h323util.h"
#include "GkStatus.h"
#include "gkconfig.h"
#include "config.h"
#if HAS_DATABASE
#include "gksql.h"
#endif
#include "clirw.h"
#include "capctrl.h"
#include "Toolkit.h"
#include "gk_const.h"

#if H323_H350
  const char *H350Section = "GkH350::Settings";
  #include <ptclib/pldap.h>
  #include "h350/h350.h"
#endif

using namespace std;

extern const char *ProxySection;
extern const char *RoutedSec;

namespace {

const PString paddingByteConfigKey("KeyFilled");
const BYTE AnyRawAddress[16] = {
	255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255
};

} /* namespace */

NetworkAddress::NetworkAddress() : m_address(0), m_netmask(0)
{
}

NetworkAddress::NetworkAddress(
	const PIPSocket::Address &addr
	) : m_address(addr), m_netmask(addr.GetSize(), AnyRawAddress)
{
}

NetworkAddress::NetworkAddress(
	const PIPSocket::Address &addr,
	const PIPSocket::Address &nm
	) : m_netmask(nm)
{
	// normalize the address
	BYTE rawdata[16];
	const unsigned sz = addr.GetSize();
	for (unsigned i = 0; i < sz; i++)
		rawdata[i] = addr[i] & nm[i];
	m_address = PIPSocket::Address(sz, rawdata);
}
	
NetworkAddress::NetworkAddress(
	const PString &str /// an address in a form A.B.C.D, A.B.C.D/24 or A.B.C.D/255.255.255.0
	)
{
	Toolkit::GetNetworkFromString(str, m_address, m_netmask);
}

unsigned NetworkAddress::GetNetmaskLen() const		
{
	unsigned len = 0;
	const unsigned sz = m_netmask.GetSize() * 8;
	const char *rawdata = m_netmask.GetPointer();
	
	
	for (int b = sz - 1; b >= 0; b--)
		if (rawdata[b >> 3] & (0x80 >> (b & 7)))
			break;
		else
			len++;
			
	return sz - len;
}

bool NetworkAddress::operator==(const NetworkAddress &addr) const
{
	if (m_address.GetSize() != addr.m_address.GetSize())
		return false;

	const unsigned sz = m_address.GetSize();
	for (unsigned i = 0; i < sz; i++)
		if (m_address[i] != addr.m_address[i] || m_netmask[i] != addr.m_netmask[i])
			return false;
			
	return true;
}

bool NetworkAddress::operator==(const PIPSocket::Address &addr) const
{
	if (m_address.GetSize() != addr.GetSize())
		return false;

	const unsigned sz = m_address.GetSize();
	for (unsigned i = 0; i < sz; i++)
		if (m_netmask[i] != 255 || m_address[i] != addr[i])
			return false;
			
	return true;
}

bool NetworkAddress::operator>>(const NetworkAddress &addr) const
{
	if (m_address.GetSize() != addr.m_address.GetSize())
		return false;

	const unsigned sz = m_address.GetSize();
	for (unsigned i = 0; i < sz; i++)
		if (m_netmask[i] != (addr.m_netmask[i] & m_netmask[i])
				|| m_address[i] != (addr.m_address[i] & m_netmask[i]))
			return false;
			
	return true;
}
	
bool NetworkAddress::operator<<(const NetworkAddress &addr) const
{
	return addr >> *this;
}

bool NetworkAddress::operator>>(const PIPSocket::Address &addr) const
{
	if (m_address.GetSize() != addr.GetSize())
		return false;

	const unsigned sz = m_address.GetSize();
	for (unsigned i = 0; i < sz; i++)
		if (m_address[i] != (addr[i] & m_netmask[i]))
			return false;
			
	return true;
}

int NetworkAddress::Compare(const NetworkAddress &addr) const
{
	int diff = m_address.GetSize() - addr.m_address.GetSize();
	if (diff == 0) {
		diff = GetNetmaskLen() - addr.GetNetmaskLen();
		if (diff == 0) {
			const unsigned sz = m_address.GetSize();
			for (unsigned i = 0; i < sz; i++) {
				diff = m_address[i] - addr.m_address[i];
				if (diff != 0)
					break;
			}
		}
	}
	return diff;
}

PString NetworkAddress::AsString() const
{
	return m_address.AsString() + "/" + PString(GetNetmaskLen());
}

bool NetworkAddress::IsAny() const
{
	return const_cast<NetworkAddress*>(this)->m_address.IsAny() ? true : false;
}

bool NetworkAddress::operator<(const NetworkAddress &addr) const
{
	return Compare(addr) < 0;
}

bool NetworkAddress::operator<=(const NetworkAddress &addr) const
{
	return Compare(addr) <= 0;
}

bool NetworkAddress::operator>(const NetworkAddress &addr) const
{
	return Compare(addr) > 0;
}

bool NetworkAddress::operator>=(const NetworkAddress &addr) const
{
	return Compare(addr) >= 0;
}

bool operator==(const PIPSocket::Address &addr, const NetworkAddress &net)
{
	return net == addr;
}

bool operator<<(const PIPSocket::Address &addr, const NetworkAddress &net)
{
	return net >> addr;
}


// class Toolkit::RouteTable::RouteEntry
Toolkit::RouteTable::RouteEntry::RouteEntry(
	const PString & net
) : PIPSocket::RouteEntry(0)
{
	destination = net.Tokenise("/", FALSE)[0];
	GetNetworkFromString(net, network, net_mask);
}

Toolkit::RouteTable::RouteEntry::RouteEntry(
	const PIPSocket::RouteEntry & re,
	const InterfaceTable & it
) : PIPSocket::RouteEntry(re)
{
	PINDEX i;
	for (i = 0; i < it.GetSize(); ++i) {
		const Address & ip = it[i].GetAddress();
		if (Compare(&ip)) {
			destination = ip;
			return;
		}
	}
	for (i = 0; i < it.GetSize(); ++i)
		if (it[i].GetName() == interfaceName) {
			destination = it[i].GetAddress();
			return;
		}
}

inline bool Toolkit::RouteTable::RouteEntry::Compare(const Address *ip) const
{
	return (*ip == destination) || ((*ip & net_mask) == network);
}

// class Toolkit::RouteTable
void Toolkit::RouteTable::InitTable()
{
	// Workaround for OS doesn't support GetRouteTable
	PIPSocket::GetHostAddress(defAddr);

	ClearTable();
	if (!CreateTable())
		return;

	// Get all the interface addresses available
	std::vector<PIPSocket::Address> home;
	PString foo = Toolkit::Instance()->GetGKHome(home);
	if (!home.empty() && home.size() > 1) {
		// Set default IP to Bind IP, if given.
		PString bind = GkConfig()->GetString("Bind", "");
		if (!bind) {
			defAddr = bind;
		} else {
			defAddr = home[0];
		}
	}

	// If we do not already have a valid entry, try and retrieve the default interface
	if (defAddr.IsLoopback() || !defAddr.IsValid()) {
		// Set default IP according to route table
		PIPSocket::Address defGW;
		PIPSocket::GetGatewayAddress(defGW);
		if (defGW.AsString() == "0.0.0.0")
			PIPSocket::GetNetworkInterface(defAddr);
		else
			defAddr = GetLocalAddress(defGW);
    }

#if PTRACING
	for (RouteEntry *entry = rtable_begin; entry != rtable_end; ++entry)
		PTRACE(2, "Network=" << entry->GetNetwork() << '/' << entry->GetNetMask() <<
			  ", IP=" << entry->GetDestination());
	PTRACE(2, "Default IP=" << defAddr);
#endif
}

void Toolkit::RouteTable::ClearTable()
{
	if (rtable_begin) {
		for (RouteEntry *r = rtable_begin; r != rtable_end; ++r)
			r->~RouteEntry();
		::free(rtable_begin);
		rtable_begin = 0;
	}
}

// can't pass a reference of Address, or STL complains...
PIPSocket::Address Toolkit::RouteTable::GetLocalAddress(const Address & addr) const
{
  // If a dynamic external IP retrieve external IP from DNS entries
	if (DynExtIP && !addr.IsRFC1918()) {
	  PIPSocket::Address extip;
	  H323TransportAddress ex = H323TransportAddress(ExtIP);
	  ex.GetIpAddress(extip);
	  return extip;
	}

	RouteEntry *entry = find_if(rtable_begin, rtable_end,
			bind2nd(mem_fun_ref(&RouteEntry::Compare), &addr));
	return (entry != rtable_end) ? entry->GetDestination() : defAddr;
}

bool Toolkit::RouteTable::CreateRouteTable(const PString & extroute)
{
	InterfaceTable if_table;
	if (!PIPSocket::GetInterfaceTable(if_table)) {
		PTRACE(1, "Error: Can't get interface table");
		return false;
	}

	PTRACE(4, "InterfaceTable:\n" << setfill('\n') << if_table << setfill(' '));
	PIPSocket::RouteTable r_table;
	if (!PIPSocket::GetRouteTable(r_table)) {
		PTRACE(1, "Error: Can't get route table");
		return false;
	}

	if (/*!extroute &&*/ AsBool(GkConfig()->GetString(ProxySection, "Enable", "0"))) {
	  for (PINDEX i = 0; i < r_table.GetSize(); ++i) {
		if (r_table[i].GetNetwork().IsRFC1918() && r_table[i].GetNetMask().AsString() != "255.255.255.255") {
		  PString intAddr = r_table[i].GetNetwork().AsString() + "/" + r_table[i].GetNetMask().AsString();
		  m_internalnetworks.resize( m_internalnetworks.size() + 1);
		  m_internalnetworks[m_internalnetworks.size() - 1] = NetworkAddress(intAddr);
		  PTRACE(2, "Internal Network Detected " << m_internalnetworks.back().AsString()); 
		} 
	  }
	}

	int i = (!extroute) ? r_table.GetSize()+1 : r_table.GetSize();

	rtable_end = rtable_begin = static_cast<RouteEntry *>(::malloc(i * sizeof(RouteEntry)));
	for (PINDEX r = 0; r < i ; ++r) {
		if (!extroute && (r==r_table.GetSize()))
			::new (rtable_end++) RouteEntry(extroute);
		else {
		  PIPSocket::RouteEntry & r_entry = r_table[r];
		  if (r_entry.GetNetMask() != INADDR_ANY) 
			::new (rtable_end++) RouteEntry(r_entry, if_table);
		}
	}

	return true;
}

bool Toolkit::VirtualRouteTable::CreateTable()
{
	PString nets = GkConfig()->GetString("NetworkInterfaces", "");
	if (!nets) {		
	   PStringArray networks(nets.Tokenise(" ,;\t", FALSE));
	   int i = networks.GetSize();
	   if (i > 0) {
          rtable_end = rtable_begin = static_cast<RouteEntry *>(::malloc(i * sizeof(RouteEntry)));
		  for (PINDEX r = 0; r < i ; ++r) 
             ::new (rtable_end++) RouteEntry(networks[r]);
       }
	   return true;
	}

	// If we have an external IP setting then load the detected Route Table and add a route for the external IP
	// If dynamic IP then only store the PString value and resolve the DNS when required.
	PString extip = GkConfig()->GetString("ExternalIP", "");
	DynExtIP = AsBool(GkConfig()->GetString("ExternalIsDynamic", "0"));

	PIPSocket::Address ext((DWORD)0);
	H323TransportAddress ex = H323TransportAddress(extip);
	ex.GetIpAddress(ext);
	if (ext.IsValid() && !ext.IsRFC1918()) {
	   ExtIP = extip;
	   PString extroute = PString();
   	   if (!DynExtIP) 
		  extroute = ext.AsString() + "/0";

	   CreateRouteTable(extroute);
	   PTRACE(1,"External IP = " << ExtIP << " dynamic " << DynExtIP);
	   return true;
	} else
        DynExtIP = false;

	return false;
}

bool Toolkit::VirtualRouteTable::IsMasquerade(PIPSocket::Address & addr) const
{
	if (!ExtIP) {
	  H323TransportAddress ex = H323TransportAddress(ExtIP);
	  ex.GetIpAddress(addr);
	  return true;
	}

	return false; 
}

// class Toolkit::ProxyCriterion
Toolkit::ProxyCriterion::ProxyCriterion() : m_enable(false)
{
}

Toolkit::ProxyCriterion::~ProxyCriterion()
{ 
}

void Toolkit::ProxyCriterion::LoadConfig(PConfig *config)
{
	m_enable = AsBool(config->GetString(ProxySection, "Enable", "0"));
	if (!m_enable) {
		PTRACE(2, "GK\tH.323 Proxy disabled");
		return;
	}

	PTRACE(2, "GK\tH.323 Proxy enabled");

	m_networks.clear();

	PStringArray networks(config->GetString(ProxySection, "InternalNetwork", "").Tokenise(" ,;\t", FALSE));

	// if no networks specified then use the detected values
	if (networks.GetSize() == 0) {
	  m_networks = Toolkit::Instance()->GetInternalNetworks();
	  for (unsigned j = 0; j < m_networks.size(); ++j) 
		PTRACE(2, "GK\tInternal Network " << j << " = " << m_networks[j].AsString());
	  return;
	} 

	  for (PINDEX i = 0; i < networks.GetSize(); ++i) {
		m_networks.resize(m_networks.size() + 1);
		m_networks[m_networks.size() - 1] = NetworkAddress(networks[i]);
		PTRACE(2, "GK\tINI Internal Network " << i << " = " << m_networks.back().AsString());
	  }
}

bool Toolkit::ProxyCriterion::Required(const Address & ip1, const Address & ip2) const
{
	return m_enable ? (m_networks.empty() || (IsInternal(ip1) != IsInternal(ip2))) : false;
}

int Toolkit::ProxyCriterion::IsInternal(const Address & ip) const
{
   // Return the network Id. Addresses may be on different internal networks
	int retval = 0;
	std::vector<NetworkAddress>::const_iterator i = m_networks.begin();
	while (i != m_networks.end()) {
		retval++;
		if (ip << *i++)
			return retval;
    }
	return 0;
}

// class Toolkit::RewriteTool

static const char *RewriteSection = "RasSrv::RewriteE164";
static const char *AliasRewriteSection = "RasSrv::RewriteAlias";
static const char *AssignedAliasSection = "RasSrv::AssignedAlias";
#ifdef h323v6
static const char *AssignedGatekeeperSection = "RasSrv::AssignedGatekeeper";
#endif

void Toolkit::RewriteData::AddSection(PConfig *config, const PString & section)
{
	PStringToString cfgs(config->GetAllKeyValues(section));
	PINDEX n_size = cfgs.GetSize();
	if (n_size > 0) {
		std::map<PString, PString, pstr_prefix_lesser> rules;
		for (PINDEX i = 0; i < n_size; ++i) {
			PString key = cfgs.GetKeyAt(i);
			PCaselessString first = PCaselessString(key[0]);				
			if (!key && (isdigit(static_cast<unsigned char>(key[0])) || (first.FindOneOf("!.%*#ABCDEFGHIGKLMNOPQRSTUVWXYZ") != P_MAX_INDEX)))			
				rules[key] = cfgs.GetDataAt(i);
		}
		// now the rules are ascendantly sorted by the keys
		if ((n_size = rules.size()) > 0) {
			// Add any existing rules to be resorted
			if (m_size > 0) {
             for (PINDEX j = 0; j < m_size; ++j) {
				 rules[Key(j)] = Value(j);
			 }
			}
			m_size = m_size + n_size;
			// replace array constructor with explicit memory allocation
			// and in-place new operators - workaround for VC compiler
//			m_RewriteKey = new PString[m_size * 2];
			m_RewriteKey = (PString*)(new BYTE[sizeof(PString) * m_size * 2]);
			m_RewriteValue = m_RewriteKey + m_size;
			std::map<PString, PString, pstr_prefix_lesser>::iterator iter = rules.begin();
			
			// reverse the order
			for (int i = m_size; i-- > 0 ; ++iter) {
//				m_RewriteKey[i] = iter->first;
				::new(m_RewriteKey + i) PString(iter->first);
//				m_RewriteValue[i] = iter->second;
				::new(m_RewriteValue + i) PString(iter->second);
			}
		}
	}
}
Toolkit::RewriteData::RewriteData(PConfig *config, const PString & section)
{
	m_RewriteKey = NULL;
	m_size= 0;
    AddSection(config, section);
}

Toolkit::RewriteData::~RewriteData()
{
//	delete [] m_RewriteKey;
	if (m_RewriteKey)
		for (int i = 0; i < m_size * 2; i++)
			(m_RewriteKey+i)->~PString();
	delete[] ((BYTE*)m_RewriteKey);
}

void Toolkit::RewriteTool::LoadConfig(
	PConfig *config
	)
{
	m_RewriteFastmatch = config->GetString(RewriteSection, "Fastmatch", "");
	m_TrailingChar = config->GetString("RasSrv::ARQFeatures", "RemoveTrailingChar", " ")[0];
	m_defaultDomain = config->GetString("Gatekeeper::Main", "DefaultDomain", "");
	delete m_Rewrite;
	m_Rewrite = new RewriteData(config, RewriteSection);
	m_Rewrite->AddSection(config,AliasRewriteSection);
}

bool Toolkit::RewriteTool::RewritePString(PString & s) const
{
	bool changed = false;

	// If URL remove the domain if default domain
	 PINDEX at = s.Find('@');
	 if (at != P_MAX_INDEX) {
		 PString num = s.Left(at);
         // Check if we have a default domain and strip it
		 if (s.Mid(at+1) == m_defaultDomain) {
		   PTRACE(2, "\tRewriteDomain: " << s << " to " << num);
		   s = num;
		   changed = true;
		 } else {
			 // Check if all numeric then is E164 then strip the domain
			 // this is used with SRV policy which includes the domain when 
			 // sending the LRQ. 
			 PINDEX j;
 		     for (j = 0; j < num.GetLength(); ++j)
			       if (!isdigit(static_cast<unsigned char>(num[j])))
			   	         break;

			 if (j >= num.GetLength()) { // is numeric
				PTRACE(2, "\tRewriteToE164: " << s << " to " << num);
                s = num;
				changed = true;
			 }
		 }
	 }

	// remove trailing character
	if (s.GetLength() > 1 && s[s.GetLength() - 1] == m_TrailingChar) {
		s = s.Left(s.GetLength() - 1);
		changed = true;
	}
	// startsWith?
	if (strncmp(s, m_RewriteFastmatch, m_RewriteFastmatch.GetLength()) != 0)
		return changed;

	PString t;
	for (PINDEX i = 0; i < m_Rewrite->Size(); ++i) {
		const char *prefix = m_Rewrite->Key(i);
		if (prefix == s){
			s = m_Rewrite->Value(i);
            return true;
		}
		const int len = MatchPrefix(s, prefix);
		// try a prefix match through all keys
		if (len > 0 || (len == 0 && prefix[0] == '!')) {
			// Rewrite to #t#. Append the suffix, too.
			// old:  01901234999
			//               999 Suffix
			//       0190        Fastmatch
			//       01901234    prefix, Config-Rule: 01901234=0521321
			// new:  0521321999

			const char *newprefix = m_Rewrite->Value(i);

			PString result;
			if (len > 0)
				result = RewriteString(s, prefix, newprefix);
			else
				result = newprefix + s;
				
			PTRACE(2, "\tRewritePString: " << s << " to " << result);
			s = result;
			changed = true;

			break;
		}
	}

	return changed;
}

// class Toolkit::GWRewriteTool

static const char *GWRewriteSection = "RasSrv::GWRewriteE164";

Toolkit::GWRewriteTool::~GWRewriteTool() {
	for (PINDEX i = 0; i < m_GWRewrite.GetSize(); ++i) {
		delete &(m_GWRewrite.GetDataAt(i));
	}
	m_GWRewrite.RemoveAll();
}

bool Toolkit::GWRewriteTool::RewritePString(PString gw, bool direction, PString &data) {

	GWRewriteEntry *gw_entry;
	PString key, value;

	// First lookup the GW in the dictionary
	gw_entry = m_GWRewrite.GetAt(gw);

	if (gw_entry == NULL)
		return false;

	std::vector<pair<PString,PString> >::iterator rule_iterator = direction
		? gw_entry->m_entry_data.first.begin() : gw_entry->m_entry_data.second.begin();
	std::vector<pair<PString,PString> >::iterator end_iterator = direction
		? gw_entry->m_entry_data.first.end() : gw_entry->m_entry_data.second.end();

	for (; rule_iterator != end_iterator; ++rule_iterator) {
	
		key = (*rule_iterator).first;
			
		const int len = MatchPrefix(data, key);
		if (len > 0 || (len == 0 && key[0] == '!')) {
			// Start rewrite
			value = (*rule_iterator).second;
				
			if (len > 0)
				value = RewriteString(data, key, value);
			else
				value = value + data;

			// Log
			PTRACE(2, "\tGWRewriteTool::RewritePString: " << data << " to " << value);

			// Finish rewrite
			data = value;
			return true;
		}
	}

	return false;
}

void Toolkit::GWRewriteTool::PrintData() {

	std::vector<pair<PString,PString> >::iterator rule_iterator;

	PTRACE(2, "GK\tLoaded per GW rewrite data:");

	if (m_GWRewrite.GetSize() == 0) {
		PTRACE(2, "GK\tNo per GW data loaded");
		return;
	}

	for (PINDEX i = 0; i < m_GWRewrite.GetSize(); ++i) {
		// In
		for (rule_iterator = m_GWRewrite.GetDataAt(i).m_entry_data.first.begin(); rule_iterator != m_GWRewrite.GetDataAt(i).m_entry_data.first.end(); ++rule_iterator) {
			PTRACE(3, "GK\t" << m_GWRewrite.GetKeyAt(i) << " (in): " << (*rule_iterator).first << " = " << (*rule_iterator).second);
		}

		// Out
		for (rule_iterator = m_GWRewrite.GetDataAt(i).m_entry_data.second.begin(); rule_iterator != m_GWRewrite.GetDataAt(i).m_entry_data.second.end(); ++rule_iterator) {
			PTRACE(3, "GK\t" << m_GWRewrite.GetKeyAt(i) << " (out): " << (*rule_iterator).first << " = " << (*rule_iterator).second);
		}
	}
	PTRACE(2, "GK\tLoaded " << m_GWRewrite.GetSize() << " GW entries with rewrite info");
}


void Toolkit::GWRewriteTool::LoadConfig(PConfig *config)
{
	std::map<PString,PString> in_strings, out_strings;
	vector<std::pair<PString,PString> > sorted_in_strings, sorted_out_strings;
	std::map<PString,PString>::reverse_iterator strings_iterator;
	pair<PString,PString> rule;

	PStringToString cfgs(config->GetAllKeyValues(GWRewriteSection));

	// Clear old config
	for (PINDEX i = 0; i < m_GWRewrite.GetSize(); ++i) {
		delete &(m_GWRewrite.GetDataAt(i));
	}
	m_GWRewrite.RemoveAll();

	PINDEX gw_size = cfgs.GetSize();
	if (gw_size > 0) {
		for (PINDEX i = 0; i < gw_size; ++i) {

			// Get the config keys
			PString key = cfgs.GetKeyAt(i);
			PString cfg_value = cfgs[key];

			in_strings.clear();
			out_strings.clear();
			sorted_in_strings.clear();
			sorted_out_strings.clear();

			// Split the config data into seperate lines
			PStringArray lines = cfg_value.Tokenise(PString(";"));

			PINDEX lines_size = lines.GetSize();

			for (PINDEX j = 0; j < lines_size; ++j) {

				// Split the config line into three strings, direction, from string, to string
				PStringArray tokenised_line = lines[j].Tokenise(PString("="));

				if (tokenised_line.GetSize() < 3) {
					PTRACE(0, "GK\tSyntax error in the GWRewriteE164 rule - missing =, rule: " 
						<< key << " => " << lines[j]
						);
					continue;
				}

				// Put into appropriate std::map
				if (tokenised_line[0] == "in")
					in_strings[tokenised_line[1]] = tokenised_line[2];
				else if (tokenised_line[0] == "out")
					out_strings[tokenised_line[1]] = tokenised_line[2];
				else
					PTRACE(0, "GK\tSyntax error in the GWRewriteE164 rule - unknown rule type ("
						<< tokenised_line[0] << ", rule: " << key << " => " << lines[j]
						);
			}

			// Put the map contents into reverse sorted vectors
			for (strings_iterator = in_strings.rbegin(); strings_iterator != in_strings.rend(); ++strings_iterator) {
				rule = *strings_iterator;
				sorted_in_strings.push_back(rule);
			}
			for (strings_iterator = out_strings.rbegin(); strings_iterator != out_strings.rend(); ++strings_iterator) {
				rule = *strings_iterator;
				sorted_out_strings.push_back(rule);
			}

			// Create the entry
			GWRewriteEntry * gw_entry = new GWRewriteEntry();
			gw_entry->m_entry_data.first = sorted_in_strings;
			gw_entry->m_entry_data.second = sorted_out_strings;

			// Add to PDictionary hash table
			m_GWRewrite.Insert(key, gw_entry);
		}
	}

	PrintData();
}

Toolkit::Toolkit() : Singleton<Toolkit>("Toolkit"), 
	m_Config(NULL), m_ConfigDirty(false),
	m_acctSessionCounter(0), m_acctSessionBase((long)time(NULL)),
	m_timerManager(new GkTimerManager()),
	m_timestampFormatStr("Cisco"),
	m_encKeyPaddingByte(-1), m_encryptAllPasswords(false),
	m_cliRewrite(NULL)
{
	srand((unsigned int)time(0));
}

Toolkit::~Toolkit()
{
	if (m_Config) {
		delete m_Config;
		PFile::Remove(m_tmpconfig);
		PFile::Remove(m_extConfigFilePath);
	}
	delete m_timerManager;
	delete m_cliRewrite;
}

Toolkit::RouteTable *Toolkit::GetRouteTable(bool real)
{
	return real ? &m_RouteTable : m_VirtualRouteTable.IsEmpty() ? &m_RouteTable : &m_VirtualRouteTable;
}

PConfig* Toolkit::Config()
{
	// Make sure the config would not be called before SetConfig
	PAssert(!m_ConfigDefaultSection, "Error: Call Config() before SetConfig()!");
	return (m_Config == NULL) ? ReloadConfig() : m_Config;
}

PConfig* Toolkit::Config(const char *section)
{
	Config()->SetDefaultSection(section);
	return m_Config;
}

PConfig* Toolkit::SetConfig(const PFilePath &fp, const PString &section)
{
	m_ConfigFilePath = fp;
	m_ConfigDefaultSection = section;

	return ReloadConfig();
}

void Toolkit::SetConfig(int act, const PString & sec, const PString & key, const PString & value)
{
	// the original config
	PConfig cfg(m_ConfigFilePath, m_ConfigDefaultSection);
	switch (act)
	{
		case 1:
			cfg.SetString(sec, key, value);
			m_Config->SetString(sec, key, value);
			break;
		case 2:
			cfg.DeleteKey(sec, key);
			m_Config->DeleteKey(sec, key);
			break;
		case 3:
			cfg.DeleteSection(sec);
			m_Config->DeleteSection(sec);
			break;
	}

	m_ConfigDirty = true;
}

PString Toolkit::GetTempDir() const
{
	PString tmpdir;
	
#ifndef _WIN32
	// check if the directory exists and is accessible (access rights)
	if (PFile::Exists("/tmp") && PFile::Access("/tmp", PFile::ReadWrite))
		tmpdir = "/tmp";
	else 
#endif
	{
		PConfig cfg(PConfig::Environment);
		
		if (cfg.HasKey("TMP"))
			tmpdir = cfg.GetString("TMP");
		else if (cfg.HasKey("TEMP"))
			tmpdir = cfg.GetString("TEMP");
		else if (cfg.HasKey("TMPDIR"))
			tmpdir = cfg.GetString("TMPDIR");
	}
	
	if (!tmpdir.IsEmpty()) {
		// strip trailing separator
		if (tmpdir[tmpdir.GetLength()-1] == PDIR_SEPARATOR)
			tmpdir = tmpdir.Left(tmpdir.GetLength()-1);
			
		// check if the directory exists and is accessible (access rights)
		if (!(PFile::Exists(tmpdir) && PFile::Access(tmpdir, PFile::ReadWrite)))
			tmpdir = PString();
	}
	
	return tmpdir;
}

void Toolkit::CreateConfig()
{
	if (m_Config != NULL)
		PFile::Remove(m_tmpconfig);

	PString tmpdir = GetTempDir();
	
#ifdef _WIN32
	if (tmpdir.IsEmpty())
		if (PFile::Access(".", PFile::ReadWrite))
			tmpdir = ".";
		else {
			const PFilePath fpath(m_ConfigFilePath);
			tmpdir = fpath.GetDirectory();
		}
#else
	if (tmpdir.IsEmpty())
		tmpdir = ".";
#endif
	
	// generate a unique name
	do {
		m_tmpconfig = tmpdir + PDIR_SEPARATOR + "gnugk.ini-" + PString(PString::Unsigned, rand()%10000);
		PTRACE(5, "GK\tTrying file name "<< m_tmpconfig << " for temp config");
	} while (PFile::Exists(m_tmpconfig));

#ifdef _WIN32
	// Does WIN32 support symlink?
	if (PFile::Copy(m_ConfigFilePath, m_tmpconfig)) {
#else
	if (symlink(m_ConfigFilePath, m_tmpconfig) == 0) {
#endif
		PConfig * testConfig = new PConfig(m_tmpconfig, m_ConfigDefaultSection);
		if( (testConfig->GetInteger("Fortytwo") == 42) || (testConfig->GetInteger("Fourtytwo") == 42)) {
			delete m_Config;
			m_Config = testConfig;
		} else {
			if (m_Config) {	// reload
				PTRACE(0, "CONFIG\tFailed to read valid config - keeping old config");
				GkStatus::Instance()->SignalStatus("Failed to read valid config - keeping old config\r\n", MIN_STATUS_TRACE_LEVEL);
			} else {	// startup
				m_Config = testConfig;	// warning will be printed a bit later			
			}
		}
	} else { // Oops! Create temporary config file failed, use the original one
		PTRACE(0, "CONFIG\tCould not create/link config to a temporary file " << m_tmpconfig);
		delete m_Config;
		m_Config = new PConfig(m_ConfigFilePath, m_ConfigDefaultSection);
	}

	if (!m_extConfigFilePath)
		PFile::Remove(m_extConfigFilePath);
	
	// generate a unique name
	do {
		m_extConfigFilePath = tmpdir + PDIR_SEPARATOR + "gnugk.ini-" + PString(PString::Unsigned, rand()%10000);
		PTRACE(5, "GK\tTrying file name "<< m_extConfigFilePath << " for external config");
	} while (PFile::Exists(m_extConfigFilePath));

	m_Config = new GatekeeperConfig(
		m_extConfigFilePath, m_ConfigDefaultSection, m_Config
		);
}

void Toolkit::ReloadSQLConfig()
{
#if HAS_DATABASE
	if (m_Config->GetSections().GetStringsIndex("SQLConfig") == P_MAX_INDEX)
		return;

	const PString driverName = m_Config->GetString("SQLConfig", "Driver", "");
	if (driverName.IsEmpty()) {
		PTRACE(0, "SQLCONF\tFailed to read config settings from SQL: no driver specified");
		return;
	}
		
	GkSQLConnection *sqlConn = GkSQLConnection::Create(driverName, "SQLCONF");
	if (sqlConn == NULL) {
		PTRACE(0, "SQLCONF\tFailed to create a connection: no driver found for "
				<< driverName << " database"
				);
		return;
	}
	
	if (!sqlConn->Initialize(m_Config, "SQLConfig")) {
		delete sqlConn;
		sqlConn = NULL;
		PTRACE(0, "SQLCONF\tFailed to read config settings from SQL: could not connect to the database");
		return;
	}

	PTRACE(3, "SQLCONF\tSQL config connection established");
	
	PString query;
	GkSQLResult* queryResult;

	query = m_Config->GetString("SQLConfig", "ConfigQuery", "");
	if (!query.IsEmpty()) {
		PTRACE(4, "SQLCONF\tLoading config key=>value pairs from SQL database");
		PStringArray params;
		params += GKName();
		queryResult = sqlConn->ExecuteQuery(query, &params);
		if (queryResult == NULL)
			PTRACE(0, "SQLCONF\tFailed to load config key=>value pairs from SQL "
				"database: timeout or fatal error"
				);
		else if (!queryResult->IsValid())
			PTRACE(0, "SQLCONF\tFailed to load config key=>value pairs from SQL "
				"database (" << queryResult->GetErrorCode() << "): " 
				<< queryResult->GetErrorMessage()
				);
		else if (queryResult->GetNumFields() < 3)
			PTRACE(0, "SQLCONF\tFailed to load config key=>value pairs from SQL "
				"database: at least 3 columns must be present in the result set"
				);
		else {
			while (queryResult->FetchRow(params))
				if (params[0].IsEmpty() || params[1].IsEmpty())
					PTRACE(1, "SQLCONF\tInvalid config key=>value pair entry found "
						"in the SQL database: '[" << params[0] << "] " 
						<< params[1] << '=' << params[1] << '\''
						);
				else {
					m_Config->SetString(params[0], params[1], params[2]);
					PTRACE(6, "SQLCONF\tConfig entry read: '[" << params[0] 
						<< "] " << params[1] << '=' << params[2] << '\''
						);
				}
			PTRACE(4, "SQLCONF\t" << queryResult->GetNumRows() 
				<< " config key=>value pairs loaded from SQL database");
		}
		delete queryResult;
		queryResult = NULL;
	}
			
// Rewrite E164
	query = m_Config->GetString("SQLConfig", "RewriteE164Query", "");
	if (!query.IsEmpty()) {
		PTRACE(4, "SQLCONF\tLoading E164 rewrite rules from SQL database");
		PStringArray params;
		params += GKName();
		queryResult = sqlConn->ExecuteQuery(query, &params);
		if (queryResult == NULL)
			PTRACE(0, "SQLCONF\tFailed to load E164 rewrite rules from SQL database: "
				"timeout or fatal error"
				);
		else if (!queryResult->IsValid())
			PTRACE(0, "SQLCONF\tFailed to load E164 rewrite rules from SQL database ("
				<< queryResult->GetErrorCode() << "): " << queryResult->GetErrorMessage()
				);
		else if (queryResult->GetNumFields() < 2)
			PTRACE(0, "SQLCONF\tFailed to load E164 rewrite rules from SQL database: "
				"at least 2 columns must be present in the result set"
				);
		else {
			while (queryResult->FetchRow(params))
				if (params[0].IsEmpty())
					PTRACE(1, "SQLCONF\tInvalid E164 rewrite rule found in the SQL "
						"database: '" << params[0] << '=' << params[1] << '\''
						);
				else {
					m_Config->SetString("RasSrv::RewriteE164", params[0], params[1]);
					PTRACE(6, "SQLCONF\tRewriteE164 rule read: '" << params[0] 
						<< '=' << params[1] << '\''
						);
				}
			PTRACE(4, "SQLCONF\t" << queryResult->GetNumRows() << " E164 rewrite rules "
				"loaded from SQL database"
				);
		}
		delete queryResult;
		queryResult = NULL;
	}

// Rewrite Alias Query
	query = m_Config->GetString("SQLConfig", "RewriteAliasQuery", "");
	if (!query.IsEmpty()) {
		PTRACE(4, "SQLCONF\tLoading rewrite rules from SQL database");
		PStringArray params;
		params += GKName();
		queryResult = sqlConn->ExecuteQuery(query, &params);
		if (queryResult == NULL)
			PTRACE(0, "SQLCONF\tFailed to load Alias rewrite rules from SQL database: "
				"timeout or fatal error"
				);
		else if (!queryResult->IsValid())
			PTRACE(0, "SQLCONF\tFailed to load Alias rewrite rules from SQL database ("
				<< queryResult->GetErrorCode() << "): " << queryResult->GetErrorMessage()
				);
		else if (queryResult->GetNumFields() < 2)
			PTRACE(0, "SQLCONF\tFailed to load Alias rewrite rules from SQL database: "
				"at least 2 columns must be present in the result set"
				);
		else {
			while (queryResult->FetchRow(params))
				if (params[0].IsEmpty())
					PTRACE(1, "SQLCONF\tInvalid Alias rewrite rule found in the SQL "
						"database: '" << params[0] << '=' << params[1] << '\''
						);
				else {
					m_Config->SetString("RasSrv::RewriteAlias", params[0], params[1]);
					PTRACE(6, "SQLCONF\tRewriteAlias rule read: '" << params[0] 
						<< '=' << params[1] << '\''
						);
				}
			PTRACE(4, "SQLCONF\t" << queryResult->GetNumRows() << " Alias rewrite rules "
				"loaded from SQL database"
				);
		}
		delete queryResult;
		queryResult = NULL;
	}

// Assigned Alias Query
	query = m_Config->GetString("SQLConfig", "AssignedAliasQuery", "");
	if (!query.IsEmpty()) {
		PTRACE(4, "SQLCONF\tLoading Assigned Alias rules from SQL database");
		PStringArray params;
		params += GKName();
		queryResult = sqlConn->ExecuteQuery(query, &params);
		if (queryResult == NULL)
			PTRACE(0, "SQLCONF\tFailed to load Assigned Alias rules from SQL database: "
				"timeout or fatal error"
				);
		else if (!queryResult->IsValid())
			PTRACE(0, "SQLCONF\tFailed to load Assigned Alias rules from SQL database ("
				<< queryResult->GetErrorCode() << "): " << queryResult->GetErrorMessage()
				);
		else if (queryResult->GetNumFields() < 2)
			PTRACE(0, "SQLCONF\tFailed to load Assigned Alias rules from SQL database: "
				"at least 2 columns must be present in the result set"
				);
		else {
			while (queryResult->FetchRow(params))
				if (params[0].IsEmpty())
					PTRACE(1, "SQLCONF\tInvalid Assigned Alias rule found in the SQL "
						"database: '" << params[0] << '=' << params[1] << '\''
						);
				else {
					m_Config->SetString("RasSrv::AssignedAlias", params[0], params[1]);
					PTRACE(6, "SQLCONF\tAssignedAlias rule read: '" << params[0] 
						<< '=' << params[1] << '\''
						);
				}
			PTRACE(4, "SQLCONF\t" << queryResult->GetNumRows() << " Assigned Alias rules "
				"loaded from SQL database"
				);
		}
		delete queryResult;
		queryResult = NULL;
	}

	query = m_Config->GetString("SQLConfig", "NeighborsQuery", "");
	if (!query.IsEmpty()) {
		PTRACE(4, "SQLCONF\tLoading neighbors from SQL database");
		PStringArray params;
		params += GKName();
		queryResult = sqlConn->ExecuteQuery(query, &params);
		if (queryResult == NULL)
			PTRACE(0, "SQLCONF\tFailed to load neighbors from SQL database: "
				"timeout or fatal error"
				);
		else if (!queryResult->IsValid())
			PTRACE(0, "SQLCONF\tFailed to load neighbors from SQL database ("
				<< queryResult->GetErrorCode() << "): " << queryResult->GetErrorMessage()
				);
		else if (queryResult->GetNumFields() < 6)
			PTRACE(0, "SQLCONF\tFailed to load neighbors from SQL database: "
				"at least 6 columns must be present in the result set"
				);
		else {
			while (queryResult->FetchRow(params)) {
				PString value;
				if (!params[5])
					value = ";" + params[5];
				if (!(params[4].IsEmpty() && value.IsEmpty()))
					value = ";" + params[4] + value;
				if (!(params[3].IsEmpty() && value.IsEmpty()))
					value = ";" + params[3] + value;
				if (!params[2])
					value = params[1] + ":" + params[2] + value;
				else
					value = params[1] + value;
				if (params[0].IsEmpty() || params[1].IsEmpty())
					PTRACE(1, "SQLCONF\tInvalid neighbor entry found in the SQL "
						"database: '" << params[0] << '=' << value << '\''
						);
				else {
					m_Config->SetString("RasSrv::Neighbors", params[0], value);
					PTRACE(6, "SQLCONF\tNeighbor entry read: '" << params[0] 
						<< '=' << value << '\''
						);
				}
			}
			PTRACE(4, "SQLCONF\t" << queryResult->GetNumRows() << " neighbor entries "
				"loaded from SQL database"
				);
		}
		delete queryResult;
		queryResult = NULL;
	}

	query = m_Config->GetString("SQLConfig", "PermanentEndpointsQuery", "");
	if (!query.IsEmpty()) {
		PTRACE(4, "SQLCONF\tLoading permanent endpoints from SQL database");
		PStringArray params;
		params += GKName();
		queryResult = sqlConn->ExecuteQuery(query, &params);
		if (queryResult == NULL)
			PTRACE(0, "SQLCONF\tFailed to load permanent endpoints from SQL "
				"database: timeout or fatal error"
				);
		else if (!queryResult->IsValid())
			PTRACE(0, "SQLCONF\tFailed to load permanent endpoints from SQL database "
				"("	<< queryResult->GetErrorCode() << "): " 
				<< queryResult->GetErrorMessage()
				);
		else if (queryResult->GetNumFields() < 4)
			PTRACE(0, "SQLCONF\tFailed to load permanent endpoints from SQL database: "
				"at least 4 columns must be present in the result set"
				);
		else {
			PString key;
			PString value;
			while (queryResult->FetchRow(params)) {
				key = params[0];
				if (!params[1])
					key += ":" + params[1];
				value = params[2];
				if (!params[3])
					value += ";" + params[3];
				if (key.IsEmpty() || value.IsEmpty())
					PTRACE(1, "SQLCONF\tInvalid permanent endpoint entry found "
						"in the SQL database: '" << key << '=' << value << '\''
						);
				else {
					m_Config->SetString("RasSrv::PermanentEndpoints", key, value);
					PTRACE(6, "SQLCONF\tPermanent endpoint read: '" << key 
						<< '=' << value << '\''
						);
				}
			}
			PTRACE(4, "SQLCONF\t" << queryResult->GetNumRows() << " permanent "
				"endpoints loaded from SQL database"
				);
		}
		delete queryResult;
		queryResult = NULL;
	}

	query = m_Config->GetString("SQLConfig", "GWPrefixesQuery", "");
	if (!query.IsEmpty()) {
		PTRACE(4, "SQLCONF\tLoading gateway prefixes from SQL database");
		PStringArray params;
		params += GKName();
		queryResult = sqlConn->ExecuteQuery(query, &params);
		if (queryResult == NULL)
			PTRACE(0, "SQLCONF\tFailed to load gateway prefixes from SQL database: "
				"timeout or fatal error"
				);
		else if (!queryResult->IsValid())
			PTRACE(0, "SQLCONF\tFailed to load gateway prefixes from SQL database ("
				<< queryResult->GetErrorCode() << "): " << queryResult->GetErrorMessage()
				);
		else if (queryResult->GetNumFields() < 2)
			PTRACE(0, "SQLCONF\tFailed to load gateway prefixes from SQL database: "
				"at least 2 columns must be present in the result set"
				);
		else {
			while (queryResult->FetchRow(params))
				if (params[0].IsEmpty() || params[1].IsEmpty())
					PTRACE(1, "SQLCONF\tInvalid gateway prefixes entry found "
						"in the SQL database: '" << params[0] << '=' 
						<< params[1] << '\''
						);
				else {
					m_Config->SetString("RasSrv::GWPrefixes", 
						params[0], params[1]
						);
					PTRACE(6, "SQLCONF\tGateway prefixes read: '" << params[0]
						<< '=' << params[1] << '\''
						);
				}
			PTRACE(4, "SQLCONF\t" << queryResult->GetNumRows() << " gateway prefixes "
				"loaded from SQL database"
				);
		}
		delete queryResult;
		queryResult = NULL;
	}
	
	delete sqlConn;
	sqlConn = NULL;
	PTRACE(3, "SQLCONF\tSQL config connection closed");
#endif // HAS_DATABASE
}

void Toolkit::PrepareReloadConfig()
{
	if (!m_ConfigDirty)
		CreateConfig();
	else // the config have been changed via status port, use it directly
		m_ConfigDirty = false;
}

PConfig* Toolkit::ReloadConfig()
{
	// make a new symlink if needed
	PrepareReloadConfig();

	// read the toolkit config values

	// read the gatekeeper name from the config file, because it might be uased as a key into the SQL config
	m_GKName = Config()->GetString("Name", "OpenH323GK");

	m_encryptAllPasswords = Toolkit::AsBool(
		Config()->GetString("EncryptAllPasswords", "0")
		);
	if (Config()->HasKey(paddingByteConfigKey))
		m_encKeyPaddingByte = Config()->GetInteger(paddingByteConfigKey, 0);
	else
		m_encKeyPaddingByte = m_encryptAllPasswords ? 0 : -1;

	ReloadSQLConfig();

	// update the gatekeeper name, in case it was set in the SQL config
	m_GKName = m_Config->GetString("Name", "OpenH323GK");
#ifdef HAS_H46018
	m_H46018Enabled	= m_Config->GetBoolean(RoutedSec, "EnableH46018", 0);
#endif

	// TODO/BUG: always call SetGKHome() on reload, even if we don't have a Home= setting
	// otherwise we won't detect new IPs on the machine
	PString GKHome(m_Config->GetString("Home", ""));
	if (m_GKHome.empty() || !GKHome)
		SetGKHome(GKHome.Tokenise(",:;", false));
	
	m_RouteTable.InitTable();
	m_VirtualRouteTable.InitTable();
	m_ProxyCriterion.LoadConfig(m_Config);
	m_Rewrite.LoadConfig(m_Config);
	m_GWRewrite.LoadConfig(m_Config);
	m_AssignedEPAliases.LoadConfig(m_Config);
#ifdef h323v6
	m_AssignedGKs.LoadConfig(m_Config);
#endif
#if HAS_DATABASE
	m_qosMonitor.LoadConfig(m_Config);
#endif

	m_timestampFormatStr = m_Config->GetString("TimestampFormat", "Cisco");

	delete m_cliRewrite;
	m_cliRewrite = new CLIRewrite;

	CapacityControl::Instance()->LoadConfig();

	LoadCauseMap(m_Config);

	LoadReasonMap(m_Config);

	ParseTranslationMap(m_receivedCauseMap, m_Config->GetString(RoutedSec, "TranslateReceivedQ931Cause", ""));
	ParseTranslationMap(m_sentCauseMap, m_Config->GetString(RoutedSec, "TranslateSentQ931Cause", ""));
	
	return m_Config;
}

void Toolkit::LoadCauseMap(
	PConfig *cfg
	)
{
	memset(m_causeMap, 0, 16);
	
	if (! Toolkit::AsBool(cfg->GetString(RoutedSec, "ActivateFailover", "0")))
		return;

	PStringArray causes(cfg->GetString(RoutedSec, "FailoverCauses", "1-15,21-127").Tokenise(", \t", FALSE));

	for (PINDEX i = 0; i < causes.GetSize(); ++i)
		if (causes[i].Find('-') == P_MAX_INDEX) {
			unsigned c = causes[i].AsUnsigned() & 0x7f;
			m_causeMap[c >> 3] |= (1UL << (c & 7));
		} else {
			PStringArray causeRange(causes[i].Tokenise("- ", FALSE));
			if (causeRange.GetSize() == 2) {
				unsigned cmin = causeRange[0].AsUnsigned() & 0x7f;
				unsigned cmax = causeRange[1].AsUnsigned() & 0x7f;
				for (; cmin <= cmax; ++cmin)
					m_causeMap[(cmin >> 3) & 0x0f] |= (1UL << (cmin & 7));
			}
		}
}

// load H.225 reason to Q.931 cause mapping
void Toolkit::LoadReasonMap(
	PConfig *cfg
	)
{
	// default to ITU-T Recommendation H.225
	unsigned DefaultH225ReasonToQ931Cause[] =	{
		34, 47, 3, 16, 88, 111, 38, 42, 28, 41, 17, 31, 16, 31, 20, 31, 47, 127,
		31, 31, 31, 127
	};
	m_H225ReasonToQ931Cause.assign(&DefaultH225ReasonToQ931Cause[0], &DefaultH225ReasonToQ931Cause[22]);

	for(int reason = 0; reason < H225_ReleaseCompleteReason::e_tunnelledSignallingRejected; reason++) {
		PString cause = cfg->GetString("H225toQ931", PString(reason), "");
		if (!cause.IsEmpty()) {
			m_H225ReasonToQ931Cause[reason] = cause.AsInteger();
		}
	}

}

bool Toolkit::MatchRegex(const PString &str, const PString &regexStr)
{
	PINDEX pos=0;
	PRegularExpression regex(regexStr, PRegularExpression::Extended);
	if(regex.GetErrorCode() != PRegularExpression::NoError) {
		PTRACE(2, "Errornous '"<< regex.GetErrorText() <<"' compiling regex: " << regexStr);
		return FALSE;
	}
	if(!regex.Execute(str, pos)) {
		PTRACE(5, "Gk\tRegex '"<<regexStr<<"' did not match '"<<str<<"'");
		return FALSE;
	}
	return TRUE;
}

#if HAS_DATABASE
Toolkit::AssignedAliases::AssignedAliases()
  : m_sqlactive(false), m_sqlConn(NULL)
{
}

Toolkit::AssignedAliases::~AssignedAliases()
{
}

bool Toolkit::AssignedAliases::LoadSQL(PConfig * cfg)
{
	delete m_sqlConn;
	PString authName = "AssignedAliases::SQL";

   if (cfg->GetSections().GetStringsIndex(authName) == P_MAX_INDEX)
		return false;

	const PString driverName = cfg->GetString(authName, "Driver", "");
	if (driverName.IsEmpty()) {
		PTRACE(0, "AliasSQL\tModule creation failed: "
			"no SQL driver selected"
			);
		PTRACE(0, "AliasSQL\tFATAL: Shutting down");
		return false;
	}
	
	m_sqlConn = GkSQLConnection::Create(driverName, authName);
	if (m_sqlConn == NULL) {
		PTRACE(0, "AliasSQL\tModule creation failed: "
			"Could not find " << driverName << " database driver"
			);
		PTRACE(0, "AliasSQL\tFATAL: Shutting down");
		return false;
	}
		
	m_query = cfg->GetString(authName, "Query", "");
	if (m_query.IsEmpty()) {
		PTRACE(0, "AliasSQL\tModule creation failed: No query configured"
			);
		PTRACE(0, "AliasSQL\tFATAL: Shutting down");
		return false;
	} else
		PTRACE(4, "AliasSQL\tQuery: " << m_query);
		
	if (!m_sqlConn->Initialize(cfg, authName)) {
		PTRACE(0, "AliasSQL\tModule creation failed: Could not connect to the database"
			);
		return false;
	}

	return true;
}

bool Toolkit::AssignedAliases::DatabaseLookup(
		const PString & alias,
		PStringArray & newAliases	
        )
{
	if (!m_sqlactive) 
		return false;

	GkSQLResult::ResultRow resultRow;
	std::map<PString, PString> params;
	params["u"] = alias;
	GkSQLResult* result = m_sqlConn->ExecuteQuery(m_query, params, m_timeout);
	if (result == NULL) {
		PTRACE(2, "AliasSQL\tQuery failed - timeout or fatal error");
		return false;
	}

	if (!result->IsValid()) {
		PTRACE(2, "AliasSQL\tQuery failed (" << result->GetErrorCode()
			<< ") - " << result->GetErrorMessage()
			);
		delete result;
		return false;
	}
	
	bool success = false;

	if (result->GetNumRows() < 1)
		PTRACE(3, "AliasSQL\tQuery returned no rows");
	else if (result->GetNumFields() < 1)
		PTRACE(2, "AliasSQL\tBad-formed query - "
			"no columns found in the result set"
			);
	else {
		PStringArray retval;
		while (result->FetchRow(retval)) {
			if (retval[0].IsEmpty()) {
				PTRACE(1, "AliasSQL\tQuery Invalid value found.");
				continue;
			} 
		    if (!success) success = true;
		    PTRACE(5, "AliasSQL\tQuery result: " << retval[0]);
		    newAliases.AppendString(retval[0]);
		}
	}
	delete result;

   return success;
}
#endif   // HAS_DATABASE

void Toolkit::AssignedAliases::LoadConfig(PConfig * m_config)
{
	gkAssignedAliases.clear();

#if HAS_DATABASE
  if (LoadSQL(m_config)) {
	  m_sqlactive = true;
  } else 
#endif
  {
   	const PStringToString kv = m_config->GetAllKeyValues(AssignedAliasSection);
	for (PINDEX i=0; i < kv.GetSize(); i++) {
		PString data = kv.GetDataAt(i);
		PStringArray datalines = data.Tokenise(" ,;\t");
		for (PINDEX j=0; j < datalines.GetSize(); j++) 
		   gkAssignedAliases.push_back(std::pair<PString, PString>(kv.GetKeyAt(i),datalines[j]));
	}
  }
}

#ifdef H323_H350

bool Toolkit::CreateH350Session(H350_Session * session)
{
	PString ldap = GkConfig()->GetString(H350Section, "ServerName", "127.0.0.1");
	PString port = GkConfig()->GetString(H350Section, "ServerPort", "389");
	PString server = ldap + ":" + port;

	PString user = GkConfig()->GetString(H350Section, "BindUserDN", "");
	PString password = Toolkit::Instance()->ReadPassword(H350Section, "BindUserPW");
    PCaselessString mode = GkConfig()->GetString(H350Section, "BindAuthMode", "simple");

	PLDAPSession::AuthenticationMethod authMethod = PLDAPSession::AuthSimple;
	if (mode == "sasl")
		authMethod = PLDAPSession::AuthSASL;
	else if (mode == "kerberos")
		authMethod = PLDAPSession::AuthKerberos;

	if (!session->Open(server)) {
	   PTRACE(4,"H350\tCannot locate H.350 Server");
	   return false;
	}
	if (!user.IsEmpty())
	    session->Bind(user,password,authMethod);

	return true;
}

bool Toolkit::AssignedAliases::QueryH350Directory(const PString & alias, PStringArray & aliases)
{
// Support Assigned Aliases
    if (!GkConfig()->GetString(H350Section, "AssignedAliases", "0"))
		   return false;

// Search the Directory
	PString search = GkConfig()->GetString(H350Section, "SearchBaseDN", "");

	H225_AliasAddress aliasaddress;
	H323SetAliasAddress(alias, aliasaddress);

	PString filter;
	switch (aliasaddress.GetTag()) {
	  case H225_AliasAddress::e_dialedDigits:
            filter = "h323IdentitydialedDigits=" + alias;
	  case H225_AliasAddress::e_h323_ID:
            filter = "h323Identityh323-ID=" + alias;
	  case H225_AliasAddress::e_url_ID:
		    filter = "h323IdentityURL-ID=" + alias;
	  default:
		  return false;
	}

	H350_Session session;
	if (!Toolkit::Instance()->CreateH350Session(&session)) {
	   PTRACE(4,"H350\tAssigned Alias: Could not connect to Server.");
	   return false;
	}

	H350_Session::LDAP_RecordList rec;
	int count = session.Search(search,filter,rec);
	if (count <= 0) {
	   PTRACE(4,"H350\tAssigned Alias: No Record Found");
	   session.Close();
	   return false;
	}

// Locate the record
	for (H350_Session::LDAP_RecordList::const_iterator x = rec.begin(); x != rec.end(); ++x) {			
       H350_Session::LDAP_Record entry = x->second;
	   PString al;
	   PINDEX i;
       if (session.GetAttribute(entry,"h323Identityh323-ID",al)) {
		   	PStringList als = al.Lines();
			for (i=0; i< als.GetSize(); i++)
				aliases.AppendString(als[i]);
	   }
       if (session.GetAttribute(entry,"h323IdentitydialedDigits",al)) {
		   	PStringList als = al.Lines();
			for (i=0; i< als.GetSize(); i++)
				aliases.AppendString(als[i]);
	   }
       if (session.GetAttribute(entry,"h323IdentityURL-ID",al)) {
		   	PStringList als = al.Lines();
			for (i=0; i< als.GetSize(); i++)
				aliases.AppendString(als[i]);
	   }
	   session.Close();
	   if (aliases.GetSize() > 0) {
		   PTRACE(2,"H350\tAssigned Alias: Located " << aliases.GetSize() << " Aliases.");
		   session.Close();
		   return true;
	   }
	}

	PTRACE(4,"H350\tAssigned Alias: No valid Assigned GK found.");
	session.Close();
	return false;
}
#endif

bool Toolkit::AssignedAliases::QueryAssignedAliases(const PString & alias, PStringArray & aliases)
{
#if HAS_DATABASE
	if (DatabaseLookup(alias, aliases))
		return true;
#endif
#ifdef H323_H350
	if (QueryH350Directory(alias,aliases))
		return true;
#endif

	return false;
}

bool Toolkit::AssignedAliases::GetAliases(const H225_ArrayOf_AliasAddress & alias, H225_ArrayOf_AliasAddress & aliaslist)
{

	if (alias.GetSize() == 0)
		    return false;

    PStringArray newaliases;
	bool found = false;
	for (PINDEX h=0; h < alias.GetSize(); h++) {
	  if (QueryAssignedAliases(H323GetAliasAddressString(alias[h]), newaliases))
		    found = true;
	}

	if (!found) {
		for (PINDEX i=0; i < alias.GetSize(); i++) {
		  PString search = H323GetAliasAddressString(alias[i]);
		  
		  for (unsigned j=0; j < gkAssignedAliases.size(); j++) {
             PTRACE(5,"Alias\tCompare " << gkAssignedAliases[j].first << " to " << search);
			  if (gkAssignedAliases[j].first == search) {
				   newaliases.AppendString(gkAssignedAliases[j].second);
				   if (!found) found = true;
			  }
		  }
		}
	}

	// Create the Assigned Alias List
	if (found) {
	  // add existing items to the end of the list
		if (aliaslist.GetSize() > 0) {
			for (PINDEX l=0; l < aliaslist.GetSize(); l++) {
			  PString a = H323GetAliasAddressString(aliaslist[l]);
			    bool located = false;
			    for (PINDEX m=0; m < newaliases.GetSize(); m++) {
                       if (newaliases[m] == a) located = true;
				}
			   if (!located)
                  newaliases.AppendString(a);
			}
		}

		aliaslist.RemoveAll();

		for (PINDEX k=0; k < newaliases.GetSize(); k++) {
			H225_AliasAddress * aliasaddress = new H225_AliasAddress;
			H323SetAliasAddress(newaliases[k], *aliasaddress);
			aliaslist.Append(aliasaddress);
		}
	}

	return found;
}

////////////////////////////////////////////////////////////////////////
#ifdef h323v6
#if HAS_DATABASE
Toolkit::AssignedGatekeepers::AssignedGatekeepers()
  : m_sqlactive(false), m_sqlConn(NULL)
{
}

Toolkit::AssignedGatekeepers::~AssignedGatekeepers()
{
}

bool Toolkit::AssignedGatekeepers::LoadSQL(PConfig * cfg)
{
	delete m_sqlConn;
	PString authName = "AssignedGatekeepers::SQL";

   if (cfg->GetSections().GetStringsIndex(authName) == P_MAX_INDEX)
		return false;

	const PString driverName = cfg->GetString(authName, "Driver", "");
	if (driverName.IsEmpty()) {
		PTRACE(0, "AssignSQL\tModule creation failed: "
			"no SQL driver selected"
			);
		PTRACE(0, "AssignSQL\tFATAL: Shutting down");
		return false;
	}
	
	m_sqlConn = GkSQLConnection::Create(driverName, authName);
	if (m_sqlConn == NULL) {
		PTRACE(0, "AssignSQL\tModule creation failed: "
			"Could not find " << driverName << " database driver"
			);
		PTRACE(0, "AssignSQL\tFATAL: Shutting down");
		return false;
	}
		
	m_query = cfg->GetString(authName, "Query", "");
	if (m_query.IsEmpty()) {
		PTRACE(0, "AssignSQL\tModule creation failed: No query configured"
			);
		PTRACE(0, "AssignSQL\tFATAL: Shutting down");
		return false;
	} else
		PTRACE(4, "AssignSQL\tQuery: " << m_query);
		
	if (!m_sqlConn->Initialize(cfg, authName)) {
		PTRACE(0, "AssignSQL\tModule creation failed: Could not connect to the database"
			);
		return false;
	}

	return true;
}

bool Toolkit::AssignedGatekeepers::DatabaseLookup(
		const PString & alias,
		const PIPSocket::Address & ipaddr,
		PStringArray & newGks	
        )
{
	if (!m_sqlactive) 
		return false;

	GkSQLResult::ResultRow resultRow;
	std::map<PString, PString> params;
	params["u"] = alias;
	params["i"] = ipaddr.AsString();
	GkSQLResult* result = m_sqlConn->ExecuteQuery(m_query, params, m_timeout);
	if (result == NULL) {
		PTRACE(2, "AssignSQL\tQuery failed - timeout or fatal error");
		return false;
	}

	if (!result->IsValid()) {
		PTRACE(2, "AssignSQL\tQuery failed (" << result->GetErrorCode()
			<< ") - " << result->GetErrorMessage()
			);
		delete result;
		return false;
	}
	
	bool success = false;

	if (result->GetNumRows() < 1)
		PTRACE(3, "AssignSQL\tQuery returned no rows");
	else if (result->GetNumFields() < 1)
		PTRACE(2, "AssignSQL\tBad-formed query - "
			"no columns found in the result set"
			);
	else {
		PStringArray retval;
		while (result->FetchRow(retval)) {
			if (retval[0].IsEmpty()) {
				PTRACE(1, "AssignSQL\tQuery Invalid value found.");
				continue;
			} 
		    if (!success) success = true;
		    PTRACE(5, "AssignSQL\tQuery result: " << retval[0]);
		    newGks.AppendString(retval[0]);
		}
	}
	delete result;

   return success;
}
#endif   // HAS_DATABASE

void Toolkit::AssignedGatekeepers::LoadConfig(PConfig * m_config)
{
	assignedGKList.clear();

#if HAS_DATABASE
  if (LoadSQL(m_config)) {
	  m_sqlactive = true;
  } else 
#endif
  {
	const PStringToString kv = m_config->GetAllKeyValues(AssignedGatekeeperSection);
	for (PINDEX i=0; i < kv.GetSize(); i++) 
		   assignedGKList.push_back(std::pair<PString, PString>(kv.GetKeyAt(i),kv.GetDataAt(i)));
  }
}

#ifdef H323_H350
bool Toolkit::AssignedGatekeepers::QueryH350Directory(const PString & alias,const PIPSocket::Address & ip, PStringArray & addresses)
{

// Support Gatekeeper discovery
    if (!GkConfig()->GetString(H350Section, "GatekeeperDiscovery", "0"))
		   return false;

// Search the Directory
	PString search = GkConfig()->GetString(H350Section, "SearchBaseDN", "");

	H225_AliasAddress aliasaddress;
	H323SetAliasAddress(alias, aliasaddress);

	PString filter;
	switch (aliasaddress.GetTag()) {
	  case H225_AliasAddress::e_dialedDigits:
            filter = "h323IdentitydialedDigits=" + alias;
	  case H225_AliasAddress::e_h323_ID:
            filter = "h323Identityh323-ID=" + alias;
	  case H225_AliasAddress::e_url_ID:
		    filter = "h323IdentityURL-ID=" + alias;
	  default:
		  return false;
	}

	H350_Session session;
	if (!Toolkit::Instance()->CreateH350Session(&session)) {
	   PTRACE(4,"H350\tAssigned GK: Could not connect to Server.");
	   return false;
	}

	H350_Session::LDAP_RecordList rec;
	int count = session.Search(search,filter,rec);
	if (count <= 0) {
	   PTRACE(4,"H350\tAssigned GK: No Record Found");
	   session.Close();
	   return false;
	}

// Locate the record
	for (H350_Session::LDAP_RecordList::const_iterator x = rec.begin(); x != rec.end(); ++x) {			
       H350_Session::LDAP_Record entry = x->second;
	   PString gk;
	   if (session.GetAttribute(entry,"h323IdentityGKDomain",gk)) {
           PTRACE(2,"H350\tAssigned GK: GK located " << gk);
		   addresses = gk.Lines();
		   session.Close();
		   return true;
	   }
	}

	PTRACE(4,"H350\tAssigned GK: No valid Assigned GK found.");
	session.Close();
	return false;
}
#endif

bool Toolkit::AssignedGatekeepers::QueryAssignedGK(const PString & alias,const PIPSocket::Address & ip, PStringArray & addresses)
{
#if HAS_DATABASE
	if (DatabaseLookup(alias,ip,addresses))
		return true;
#endif
#ifdef H323_H350
	if (QueryH350Directory(alias,ip,addresses))
		return true;
#endif

	return false;
}

#ifdef hasSRV
static PString DNStoIP(const PString & dns)
{
   H323TransportAddress iface(dns);
   PIPSocket::Address ip;
   WORD port = GK_DEF_UNICAST_RAS_PORT;
   iface.GetIpAndPort(ip, port); 
   return PString(ip + ":" + port);
}
#endif

bool Toolkit::AssignedGatekeepers::GetAssignedGK(const PString & alias,const PIPSocket::Address & ip, H225_ArrayOf_AlternateGK & gklist)
{
	bool found = false;
	PStringArray assignedGK;
	found = QueryAssignedGK(alias,ip,assignedGK);

	if (!found) {
		  for (unsigned j=0; j < assignedGKList.size(); j++) {
		    PString match = assignedGKList[j].first.Trim();
			if (match.Left(1) != "^") {
			  // Do prefix match
			  if (MatchPrefix(alias,assignedGKList[j].first)) {
				   assignedGK.AppendString(assignedGKList[j].second);
				   if (!found) found = true;
			  }
			} else {
	          // Do Regex match for IP address
			  if (MatchRegex(ip.AsString(),match)) {
				   assignedGK.AppendString(assignedGKList[j].second);
				   if (!found) found = true;
		      }
			}
		  }
	}

	if (found) {
      PStringArray ipaddresses;
		for (PINDEX k=0; k < assignedGK.GetSize(); k++) {
           PString number = assignedGK[k];

			if (IsIPAddress(number)) 
				ipaddresses.AppendString(number);
#ifdef hasSRV
			else {
				PString xnum = assignedGK[k];
				if (xnum.Left(5) != "h323:") 
						xnum = "h323:user@" + xnum;

				PStringList str;
				if (PDNS::LookupSRV(xnum,"_h323rs._udp.",str)) {
					PTRACE(4, "AssignGK\t" << str.GetSize() << " SRV Records found" );
					for (PINDEX i = 0; i < str.GetSize(); i++) {
						PString newhost = str[i].Right(str[i].GetLength()-5);
						PTRACE(4, "AssignGK\tDNS SRV converted GK address " << number << " to " << newhost );
						ipaddresses.AppendString(newhost);
					}
				} else {
						ipaddresses.AppendString(DNStoIP(number));
				}
			}
#endif   
		}

		for (PINDEX k=0; k < ipaddresses.GetSize(); k++) {
           PString num = ipaddresses[k];
           WORD port = GK_DEF_UNICAST_RAS_PORT;
		   PStringArray tokens = num.Tokenise(":", FALSE);
		   if (tokens.GetSize() == 2) 
               port = (WORD)tokens[1].AsUnsigned();
	        
		   H225_AlternateGK * alt = new H225_AlternateGK;
		     alt->m_rasAddress = SocketToH225TransportAddr(PIPSocket::Address(tokens[0]),port);
		     alt->m_needToRegister = true;
		     alt->m_priority = k;
		   gklist.Append(alt);
		}
	}
		
	return found;
}
#endif

/////////////////////////////////////////////////////////////////////////////////////////

#if HAS_DATABASE
Toolkit::QoSMonitor::QoSMonitor()
  : m_sqlactive(false), m_sqlConn(NULL)
{
}

Toolkit::QoSMonitor::~QoSMonitor()
{
}
	    
void Toolkit::QoSMonitor::LoadConfig(PConfig * cfg)
{
	delete m_sqlConn;
	PString authName = "GkQoSMonitor::SQL";

   if (cfg->GetSections().GetStringsIndex(authName) == P_MAX_INDEX)
		return;

	const PString driverName = cfg->GetString(authName, "Driver", "");
	if (driverName.IsEmpty()) {
		PTRACE(0, "QoSSQL\tModule creation failed: "
			"no SQL driver selected"
			);
		PTRACE(0, "QoSSQL\tFATAL: Shutting down");
		return;
	}
	
	m_sqlConn = GkSQLConnection::Create(driverName, authName);
	if (m_sqlConn == NULL) {
		PTRACE(0, "QoSSQL\tModule creation failed: "
			"Could not find " << driverName << " database driver"
			);
		PTRACE(0, "QoSSQL\tFATAL: Shutting down");
		return;
	}
		
	m_query = cfg->GetString(authName, "Query", "");
	if (m_query.IsEmpty()) {
		PTRACE(0, "QoSSQL\tModule creation failed: No query configured"
			);
		PTRACE(0, "QoSSQL\tFATAL: Shutting down");
		return;
	} else
		PTRACE(4, "QoSSQL\tQuery: " << m_query);
		
	if (!m_sqlConn->Initialize(cfg, authName)) {
		PTRACE(0, "QoSSQL\tModule creation failed: Could not connect to the database"
			);
		return;
	}

	m_sqlactive = true;

}

bool Toolkit::QoSMonitor::PostRecord(const std::map<PString, PString>& params)
{
	if (!m_sqlactive) 
		return false;

	GkSQLResult* result = m_sqlConn->ExecuteQuery(m_query, params, m_timeout);

	if (result == NULL) {
		PTRACE(2, "QoSSQL\tFailed to store QoS Data: Timeout or fatal error");
		return false;
	}
	
	if (result) {
		if (result->IsValid()) {
			if (result->GetNumRows() < 1) {
				PTRACE(4, "QoSSQL\tFailed to store QoS Data: No rows have been updated"
					);
				delete result;
				return false;
			}
		} else {
			PTRACE(2, "QoSSQL\tfailed to store QoS Data: Err(" << result->GetErrorCode() << ") "
				<< result->GetErrorMessage() );
			delete result;
			return false;
		}
	}
    
	delete result;
	return true;
}
#endif

/////////////////////////////////////////////////////////////////////////////////////////

bool Toolkit::RewriteE164(H225_AliasAddress & alias)
{
	if ((alias.GetTag() != H225_AliasAddress::e_dialedDigits) &&
         (alias.GetTag() != H225_AliasAddress::e_h323_ID) &&
		 (alias.GetTag() != H225_AliasAddress::e_url_ID)) {
		if (alias.GetTag() != H225_AliasAddress::e_partyNumber)
			return false;
		H225_PartyNumber & partyNumber = alias;
		if (partyNumber.GetTag() != H225_PartyNumber::e_e164Number && partyNumber.GetTag() != H225_PartyNumber::e_privateNumber)
			return false;
	}
	
	PString E164 = ::AsString(alias, FALSE);

	bool changed = RewritePString(E164);
	if (changed) {
		if (alias.GetTag() == H225_AliasAddress::e_dialedDigits)
			H323SetAliasAddress(E164, alias, alias.GetTag());
		else {
			if (alias.GetTag() == H225_AliasAddress::e_partyNumber) {
				H225_PartyNumber & partyNumber = alias;
				if (partyNumber.GetTag() == H225_PartyNumber::e_e164Number) {
					H225_PublicPartyNumber &number = partyNumber;
					number.m_publicNumberDigits = E164;
				} else if (partyNumber.GetTag() == H225_PartyNumber::e_privateNumber) {
					H225_PrivatePartyNumber &number = partyNumber;
					number.m_privateNumberDigits = E164;
				}
			}
		}
	}
	
	return changed;
}

bool Toolkit::RewriteE164(H225_ArrayOf_AliasAddress & aliases)
{
	bool changed = false;
	for (PINDEX n = 0; n < aliases.GetSize(); ++n)
		changed |= RewriteE164(aliases[n]);
	return changed;
}

bool Toolkit::GWRewriteE164(PString gw, bool direction, H225_AliasAddress &alias) {

	PString E164;
	bool changed;

	if (alias.GetTag() != H225_AliasAddress::e_dialedDigits) {
		if (alias.GetTag() != H225_AliasAddress::e_partyNumber)
			return false;
		H225_PartyNumber &partyNumber = alias;
		if (partyNumber.GetTag() != H225_PartyNumber::e_e164Number && partyNumber.GetTag() != H225_PartyNumber::e_privateNumber)
			return false;
	}

	E164 = ::AsString(alias, FALSE);
	changed = GWRewritePString(gw,direction,E164);

	if (changed) {
		if (alias.GetTag() == H225_AliasAddress::e_dialedDigits)
			H323SetAliasAddress(E164, alias, alias.GetTag());
		else {
			H225_PartyNumber &partyNumber = alias;
			if (partyNumber.GetTag() == H225_PartyNumber::e_e164Number) {
				H225_PublicPartyNumber &number = partyNumber;
				number.m_publicNumberDigits = E164;
			} else if (partyNumber.GetTag() == H225_PartyNumber::e_privateNumber) {
				H225_PrivatePartyNumber &number = partyNumber;
				number.m_privateNumberDigits = E164;
			}
		}
	}

	return changed;
}

bool Toolkit::GWRewriteE164(PString gw, bool direction, H225_ArrayOf_AliasAddress &aliases) {

	bool changed;
	PINDEX n;

	changed = false;
	for (n = 0; n < aliases.GetSize(); ++n) {
		changed |= GWRewriteE164(gw,direction,aliases[n]);
	}

	return changed;
}

bool Toolkit::isBehindNAT(PIPSocket::Address & externalIP) const {

   return (m_VirtualRouteTable.IsMasquerade(externalIP));
}

std::vector<NetworkAddress> Toolkit::GetInternalNetworks() {

    return GkConfig()->HasKey("ExternalIP") ? m_VirtualRouteTable.GetInternalNetworks() : m_RouteTable.GetInternalNetworks(); 
}

PString Toolkit::GetGKHome(vector<PIPSocket::Address> & GKHome) const
{
	GKHome = m_GKHome;
	PString result;
	int hsize = GKHome.size();
	for (int i = 0; i < hsize; ++i) {
		result += GKHome[i].AsString();
		if (i < hsize - 1)
			result += ",";
	}
	return result;
}

void Toolkit::SetGKHome(const PStringArray & home)
{
	std::vector<PIPSocket::Address>::iterator begin;
	m_GKHome.clear();
	int n, i, size = home.GetSize();
	if (size > 0)
		for (n = 0; n < size; ++n)
			m_GKHome.push_back(home[n]);

	PIPSocket::InterfaceTable it;
	if (PIPSocket::GetInterfaceTable(it)) {
		int is = it.GetSize();
		if (size > 0) {
			// check if the interface is valid
			for (n = 0; n < size; ++n) {
				for (i = 0; i < is; ++i)
					if (m_GKHome[n] == it[i].GetAddress())
						break;
				if (i == is) {
					PTRACE(1, "GK\tAddress " << m_GKHome[n] << " not found"
						" in the PWLib interface table"
						);
					//begin = m_GKHome.begin();
					//copy(begin + n + 1, begin + size, begin + n);
					//--size, --n;
				}
			}
		}
		if (size == 0) {
			m_GKHome.clear();
			size = is;
			for (n = 0; n < size; ++n)
				m_GKHome.push_back(it[n].GetAddress());
		}
		// remove INADDR_ANY
		for (n = 0; n < size; ++n)
			if (m_GKHome[n] == INADDR_ANY) {
				begin = m_GKHome.begin();
				copy(begin + n + 1, begin + size, begin + n);
				--size, --n;
			}
	}

	// remove duplicate interfaces
	for (n = 0; n < size; ++n)
		for (i = 0; i < n; ++i)
			if (m_GKHome[n] == m_GKHome[i]) {
				begin = m_GKHome.begin();
				copy(begin + n + 1, begin + size, begin + n);
				--size, --n;
				break;
			}

	m_GKHome.resize(size);
	// put the default IP to the first
	begin = find(m_GKHome.begin(), m_GKHome.end(), m_RouteTable.GetLocalAddress());
	if (begin != m_GKHome.end())
		swap(m_GKHome[0], *begin);
}

int
Toolkit::GetInternalExtensionCode( const unsigned &country,
				   const unsigned &extension,
				   const unsigned &manufacturer) const
{
	switch (country) {
	case t35cOpenOrg:
		switch (manufacturer) {
		case t35mOpenOrg:
			switch (extension) {
				case t35eFailoverRAS: return iecFailoverRAS;
			}
			break;
		}
		break;
		
	case t35cPoland:
		switch (manufacturer) {
		case t35mGnuGk:
			switch (extension) {
			case t35eNeighborId:
				return iecNeighborId;
			case t35eNATTraversal:
				return iecNATTraversal;
			}
			break;
		}
		break;
	}

	// default for all other cases
	return iecUnknown;
}

int Toolkit::GetInternalExtensionCode(const H225_H221NonStandard& data) const
{
	return GetInternalExtensionCode(data.m_t35CountryCode,
			data.m_t35Extension,
			data.m_manufacturerCode);
}

bool Toolkit::AsBool(const PString & str)
{
	if (str.IsEmpty())
		return false;
	const unsigned char c = (unsigned char)tolower(str[0]);
	return ( c=='t' || c=='1' || c=='y' || c=='a' );
}

void Toolkit::GetNetworkFromString(
	const PString &s,
	PIPSocket::Address &network,
	PIPSocket::Address &netmask
	)
{
	if (s *= "ALL") {
		network = netmask = INADDR_ANY;
		return;
	}

	PINDEX slashPos = s.Find('/');
	if (slashPos == P_MAX_INDEX) {
		// a single IP
		static BYTE fullNetMask[16] = { 
			255, 255, 255, 255, 255, 255, 255, 255,
			255, 255, 255, 255, 255, 255, 255, 255
			};
		network = PIPSocket::Address(s);
		netmask = PIPSocket::Address(network.GetSize(), fullNetMask);
	} else {
		network = PIPSocket::Address(s.Left(slashPos));
		
		const PString netmaskString = s.Mid(slashPos + 1);
		BYTE rawData[16];
		
		if (netmaskString.FindOneOf(".:") != P_MAX_INDEX) {
			// netmask as a network address
			netmask = PIPSocket::Address(netmaskString);
		} else {
			// netmask as an integer
			const DWORD netmaskLen = netmaskString.AsUnsigned();
			for (unsigned b = 0; b < (unsigned)(network.GetSize() * 8); b++)
				if (b < netmaskLen)
					rawData[b >> 3] |= 0x80U >> (b & 7);
				else
					rawData[b >> 3] &= ~(0x80U >> (b & 7));
			netmask = PIPSocket::Address(network.GetSize(), rawData);
		}
		
		// normalize the address
		for (unsigned b = 0; b < (unsigned)(network.GetSize()); b++)
			rawData[b] = network[b] & netmask[b];
				
		network = PIPSocket::Address(network.GetSize(), rawData);
	}
}

PString Toolkit::GenerateAcctSessionId()
{
	PWaitAndSignal lock( m_acctSessionMutex );
	return psprintf(PString("%08x%08x"),m_acctSessionBase,++m_acctSessionCounter);
}

PString Toolkit::AsString(
	const PTime& tm, /// timestamp to convert into a string
	const PString& formatStr /// format string to use
	)
{
	PString fmtStr = !formatStr ? formatStr : m_timestampFormatStr;
	if (fmtStr.IsEmpty())
		return PString();

	if (fmtStr *= "Cisco")
		fmtStr = "%H:%M:%S.%u %Z %a %b %d %Y";
	else if (fmtStr *= "ISO8601")
		return tm.AsString(PTime::LongISO8601);
	else if (fmtStr *= "RFC822")
		return tm.AsString(PTime::RFC1123);
	else if (fmtStr *= "MySQL" )
		fmtStr = "%Y-%m-%d %H:%M:%S";
	
	struct tm _tm;
	struct tm* tmptr = &_tm;
	time_t t = tm.GetTimeInSeconds();

#ifndef _WIN32
	if (localtime_r(&t, tmptr) != tmptr) {
#else
	tmptr = localtime(&t);
	if (tmptr  == NULL) {
#endif
		PTRACE(0, "TOOLKIT\tCould not apply timestamp formatting - using default");
		return tm.AsString( "hh:mm:ss.uuu z www MMM d yyyy" );
	}

	// replace %u with microseconds - this is our extension
	PINDEX i = 0;
	PINDEX length = fmtStr.GetLength();
	do {
		i = fmtStr.Find("%u", i);
		if (i != P_MAX_INDEX) {
			if (i > 0 && fmtStr[i-1] == '%') {
				i += 2;
				continue;
			}
			const PString us(PString::Printf, "%03d", (unsigned)tm.GetMicrosecond());
			fmtStr.Splice(us, i, 2);
			length += us.GetLength();
			i += us.GetLength();
			length -= 2;
		}
	} while (i != P_MAX_INDEX && i < length);
	
	PString buf;
	
	buf.SetSize(128);
	if (strftime(buf.GetPointer(), 128, (const char*)fmtStr, tmptr) == 0) {
		PTRACE(0, "TOOLKIT\tCould not apply timestamp formatting - using default");
		return tm.AsString( "hh:mm:ss.uuu z www MMM d yyyy" );
	}
	
	buf.MakeMinimumSize();
	return buf;
}

PString Toolkit::ReadPassword(
	const PString &cfgSection, /// config section to read
	const PString &cfgKey, /// config key to read an encrypted password from
	bool forceEncrypted
	)
{
	if (cfgSection.IsEmpty() || cfgKey.IsEmpty())
		return PString();
		
	PConfig* const cfg = Config();
	if (!cfg->HasKey(cfgSection, cfgKey))
		return PString();

	int paddingByte = m_encKeyPaddingByte;
	if (cfg->HasKey(cfgSection, paddingByteConfigKey)) {
		paddingByte = cfg->GetInteger(cfgSection, paddingByteConfigKey, 0);
	}

	if (paddingByte == -1) {
		if (forceEncrypted || m_encryptAllPasswords) {
			paddingByte = 0;
		} else {
			return cfg->GetString(cfgSection, cfgKey, "");
		}
	}

	PTEACypher::Key encKey;
	memset(&encKey, paddingByte, sizeof(encKey));

	const size_t keyLen = cfgKey.GetLength();
	if (keyLen > 0) {
		memcpy(&encKey, (const char*)cfgKey, min(keyLen, sizeof(encKey)));
	}

	PTEACypher cypher(encKey);
	PString s;
	if (!cypher.Decode(cfg->GetString(cfgSection, cfgKey, ""), s)) {
		PTRACE(1, "GK\tFailed to decode config password for [" << cfgSection
			<< "] => " << cfgKey);
	}
	return s;
}

void Toolkit::RewriteCLI(
	SetupMsg &msg
	) const
{
	m_cliRewrite->InRewrite(msg);
}

void Toolkit::RewriteCLI(
	SetupMsg &msg,
	SetupAuthData &authData,
	const PIPSocket::Address &addr
	) const
{
	m_cliRewrite->OutRewrite(msg, authData, addr);
}

void Toolkit::SetRerouteCauses(
	unsigned char *causeMap
	)
{
	memcpy(causeMap, m_causeMap, 128/8);
}


unsigned Toolkit::MapH225ReasonToQ931Cause(
	int reason
	)
{
	if( reason < 0 || reason > H225_ReleaseCompleteReason::e_tunnelledSignallingRejected )
		return 0;
	else
		return m_H225ReasonToQ931Cause[reason];
}

void Toolkit::ParseTranslationMap(std::map<unsigned, unsigned> & cause_map, const PString & ini) const
{
	cause_map.clear();
	PStringArray pairs(ini.Tokenise(",", false));
	for (PINDEX i = 0; i < pairs.GetSize(); ++i) {
		PStringArray causes(pairs[i].Tokenise(":=", false));
		if (causes.GetSize() == 2) {
			cause_map.insert(pair<unsigned, unsigned>(causes[0].AsInteger(), causes[1].AsInteger()));
		} else {
			PTRACE(1, "Syntax error in cause mapping: " << causes[i]);
		}
	}
}

unsigned Toolkit::TranslateReceivedCause(unsigned cause) const
{
	std::map<unsigned, unsigned>::const_iterator i = m_receivedCauseMap.find(cause);
	if (i != m_receivedCauseMap.end())
		return i->second;
	else
		return cause;
}

unsigned Toolkit::TranslateSentCause(unsigned cause) const
{
	std::map<unsigned, unsigned>::const_iterator i = m_sentCauseMap.find(cause);
	if (i != m_sentCauseMap.end())
		return i->second;
	else
		return cause;
}

#ifdef OpenH323Factory
PStringList Toolkit::GetAuthenticatorList()
{
  PString auth = GkConfig()->GetString("Gatekeeper::Main", "Authenticators", "");
  PStringArray authlist(auth.Tokenise(" ,;\t"));

  return authlist;
}
#endif
