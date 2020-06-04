//
// Copyright (c) 2017-2020, Manticore Software LTD (http://manticoresearch.com)
// Copyright (c) 2001-2016, Andrew Aksyonoff
// Copyright (c) 2008-2016, Sphinx Technologies Inc
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#include "sphinx.h"
#include "sphinxstd.h"
#include "sphinxint.h"
#include "searchdaemon.h"
#include "searchdha.h"
#include "searchdtask.h"
#include "coroutine.h"

#include <utility>
#include <atomic>
#include <errno.h>

#if !USE_WINDOWS
	#include <netinet/in.h>
	#include <netdb.h>
#else
	#include <WinSock2.h>
	#include <MSWSock.h>
	#include <WS2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#endif

#if HAVE_EVENTFD
	#include <sys/eventfd.h>
#endif

#if HAVE_GETADDRINFO_A
	#include <signal.h>
#endif

#if HAVE_KQUEUE
	#include <sys/types.h>
	#include <sys/event.h>
	#include <sys/time.h>
#endif

int64_t			g_iPingIntervalUs		= 0;		// by default ping HA agents every 1 second
DWORD			g_uHAPeriodKarmaS	= 60;		// by default use the last 1 minute statistic to determine the best HA agent

int				g_iPersistentPoolSize	= 0;

static auto& g_bShutdown = sphGetShutdown();
static auto& g_iTFO = sphGetTFO ();

CSphString HostDesc_t::GetMyUrl() const
{
	CSphString sName;
	switch ( m_iFamily )
	{
	case AF_INET: sName.SetSprintf ( "%s:%u", m_sAddr.cstr(), m_iPort ); break;
	case AF_UNIX: sName = m_sAddr; break;
	}
	return sName;
}

#define VERBOSE_NETLOOP 0

#if VERBOSE_NETLOOP
	#define sphLogDebugA( ... ) TimePrefixed::LogDebugv ("A ", __VA_ARGS__)
	#define sphLogDebugL( ... ) TimePrefixed::LogDebugv ("L ", __VA_ARGS__)
#else
#if USE_WINDOWS
#pragma warning(disable:4390)
#endif
	#define sphLogDebugA( ... )
	#define sphLogDebugL( ... )
#endif

/////////////////////////////////////////////////////////////////////////////
// HostDashboard_t
//
// Dashboard contains generic statistic for a host like query time, answer time,
// num of errors a row, and all this data in retrospective on time
/////////////////////////////////////////////////////////////////////////////
HostDashboard_t::HostDashboard_t ( const HostDesc_t & tHost )
{
	assert ( !tHost.m_pDash );
	m_tHost.CloneFromHost ( tHost );
	m_iLastQueryTime = m_iLastAnswerTime = sphMicroTimer ();
	for ( auto & dMetric : m_dPeriodicMetrics )
		dMetric.m_dMetrics.Reset ();
}

HostDashboard_t::~HostDashboard_t ()
{
	SafeDelete ( m_pPersPool );
}

int64_t HostDashboard_t::EngageTime () const NO_THREAD_SAFETY_ANALYSIS
{
	// m_dMetricsLock is essential when we read/update all metrics a time, atomically.
	// (for just one m_iLastAnswerTime it is enough that it is atomic itself, no lock necessary)
	return m_iLastAnswerTime + g_iPingIntervalUs;
}

DWORD HostDashboard_t::GetCurSeconds()
{
	int64_t iNow = sphMicroTimer()/1000000;
	return DWORD ( iNow & 0xFFFFFFFF );
}

bool HostDashboard_t::IsHalfPeriodChanged ( DWORD * pLast )
{
	assert ( pLast );
	DWORD uSeconds = GetCurSeconds();
	if ( ( uSeconds - *pLast )>( g_uHAPeriodKarmaS / 2 ) )
	{
		*pLast = uSeconds;
		return true;
	}
	return false;
}

MetricsAndCounters_t& HostDashboard_t::GetCurrentMetrics ()
{
	DWORD uCurrentPeriod = GetCurSeconds()/g_uHAPeriodKarmaS;
	auto & dCurrentMetrics = m_dPeriodicMetrics[uCurrentPeriod % STATS_DASH_PERIODS];
	if ( dCurrentMetrics.m_uPeriod!=uCurrentPeriod ) // we have new or reused stat
	{
		dCurrentMetrics.m_dMetrics.Reset ();
		dCurrentMetrics.m_uPeriod = uCurrentPeriod;
	}

	return dCurrentMetrics.m_dMetrics;
}

void HostDashboard_t::GetCollectedMetrics ( HostMetricsSnapshot_t& dResult, int iPeriods ) const
{
	DWORD uSeconds = GetCurSeconds();

	if ( (uSeconds % g_uHAPeriodKarmaS) < (g_uHAPeriodKarmaS/2) )
		++iPeriods;
	iPeriods = Min ( iPeriods, STATS_DASH_PERIODS );

	DWORD uCurrentPeriod = uSeconds/g_uHAPeriodKarmaS;
	MetricsAndCounters_t tAccum;
	tAccum.Reset ();

	CSphScopedRLock tRguard ( m_dMetricsLock );
	for ( ; iPeriods>0; --iPeriods, --uCurrentPeriod )
		// it might be no queries at all in the fixed time
		if ( m_dPeriodicMetrics[uCurrentPeriod % STATS_DASH_PERIODS].m_uPeriod==uCurrentPeriod )
			tAccum.Add ( m_dPeriodicMetrics[uCurrentPeriod % STATS_DASH_PERIODS].m_dMetrics );

	for ( int i = 0; i<eMaxAgentStat; ++i )
		dResult[i] = tAccum.m_dCounters[i];
	for ( int i = 0; i<ehMaxStat; ++i )
		dResult[i+eMaxAgentStat] = tAccum.m_dMetrics[i];
}

/////////////////////////////////////////////////////////////////////////////
// PersistentConnectionsPool_c
//
// This is pool of sockets which can be rented/returned by workers.
// returned socket may be in connected state, so later usage will not require
// connection, but can work immediately (in persistent mode).
/////////////////////////////////////////////////////////////////////////////

// modify size of the pool
// (actually we just need to close any extra sockets not fit in new limit. And do nothing otherwize).
void PersistentConnectionsPool_c::ReInit ( int iPoolSize )
{
	assert ( iPoolSize >0 );
	// close any extra sockets which are not fit in new limit
	ScopedMutex_t tGuard ( m_dDataLock );
	m_iLimit = iPoolSize;
	m_dSockets.Reserve ( m_iLimit );
	while ( m_iFreeWindow>m_iLimit )
	{
		--m_iFreeWindow;
		int iSock = m_dSockets[Step ( &m_iRit )];
		if ( iSock>=0 )
			sphSockClose ( iSock );
	}

}

// move the iterator to the next item, or loop the ring.
int PersistentConnectionsPool_c::Step ( int* pVar )
{
	assert ( pVar );
	int iRes = *pVar++;
	if ( *pVar>=m_dSockets.GetLength () )
		*pVar = 0;
	return iRes;
}

// Rent first try to return the sockets which already were in work (i.e. which are connected)
// If no such socket available and the limit is not reached, it will add the new one.
int PersistentConnectionsPool_c::RentConnection ()
{
	ScopedMutex_t tGuard ( m_dDataLock );
	if ( m_iFreeWindow>0 )
	{
		--m_iFreeWindow;
		return m_dSockets[Step ( &m_iRit )];
	}
	if ( m_dSockets.GetLength () >= m_iLimit )
		return -2; // no more slots available;

	// this branch will be executed only during initial 'heating'
	m_dSockets.Add ( -1 );
	return -1;
}

// write given socket into the pool.
void PersistentConnectionsPool_c::ReturnConnection ( int iSocket )
{
	ScopedMutex_t tGuard ( m_dDataLock );

	// overloaded pool
	if ( m_iFreeWindow >= m_dSockets.GetLength () )
	{
		// no place at all (i.e. if pool was resized, but some sockets rented before,
		// and now they are returned, but we have no place for them)
		if ( m_dSockets.GetLength () >= m_iLimit )
		{
			sphSockClose ( iSocket );
			return;
		}
		// add the place for one more returned socket
		m_dSockets.Add ();
		m_iWit = m_dSockets.GetLength ()-1;
	}
	++m_iFreeWindow;
	if ( m_bShutdown )
	{
		sphSockClose ( iSocket );
		iSocket = -1;
	}
	// if there were no free sockets until now, point the reading iterator to just released socket
	if ( m_iFreeWindow==1 )
		m_iRit = m_iWit;
	m_dSockets[Step ( &m_iWit )] = iSocket;
}

// close all the sockets in the pool.
void PersistentConnectionsPool_c::Shutdown ()
{
	ScopedMutex_t tGuard ( m_dDataLock );
	m_bShutdown = true;
	for ( int i = 0; i<m_iFreeWindow; ++i )
	{
		int& iSock = m_dSockets[Step ( &m_iRit )];
		if ( iSock>=0 )
		{
			sphSockClose ( iSock );
			iSock = -1;
		}
	}
}

void ClosePersistentSockets()
{
	Dashboard::GetActiveHosts().Apply ([] (HostDashboard_t * &pHost) { SafeDelete ( pHost->m_pPersPool ); });
}

// check whether sURL contains plain ip-address, and so, m.b. no need to resolve it many times.
// ip-address assumed as string from four .-separated positive integers, each in range 0..255
static bool IsIpAddress ( const char * sURL )
{
	StrVec_t dParts;
	sphSplit ( dParts, sURL, "." );
	if ( dParts.GetLength ()!=4 )
		return false;
	for ( auto &dPart : dParts )
	{
		dPart.Trim ();
		auto iLen = dPart.Length ();
		for ( auto i = 0; i<iLen; ++i )
			if ( !isdigit ( dPart.cstr ()[i] ) )
				return false;
		auto iRes = atoi ( dPart.cstr () );
		if ( iRes<0 || iRes>255 )
			return false;
	}
	return true;
}

/// Set flag m_bNeedResolve if address is AF_INET, host is not empty and not plain IP address,
/// and also if global flag for postponed resolving is set.
/// Otherwise resolve address now (if appliable) and then forbid any name resolving in future.
bool ResolveAddress ( AgentDesc_t &tAgent, const WarnInfo_c & tInfo )
{
	tAgent.m_bNeedResolve = false;
	if ( tAgent.m_iFamily!=AF_INET )
		return true;

	if ( tAgent.m_sAddr.IsEmpty () )
		return tInfo.ErrSkip ( "invalid host name 'empty'" );

	if ( IsIpAddress ( tAgent.m_sAddr.cstr () ) )
	{
		tAgent.m_uAddr = sphGetAddress ( tAgent.m_sAddr.cstr (), false, true );
		if ( tAgent.m_uAddr )
			return true;

		// got plain ip and couldn't resolve it? Hm, strange...
		if ( !g_bHostnameLookup )
			return tInfo.ErrSkip ( "failed to lookup host name '%s' (error=%s)", tAgent.m_sAddr.cstr ()
								   , sphSockError () );
	}

	// if global flag set, don't resolve right now, it will be done on place.
	tAgent.m_bNeedResolve = g_bHostnameLookup;
	if ( tAgent.m_bNeedResolve )
		return true;

	tAgent.m_uAddr = sphGetAddress ( tAgent.m_sAddr.cstr () );
	if ( tAgent.m_uAddr )
		return true;

	return tInfo.ErrSkip ( "failed to lookup host name '%s' (error=%s)", tAgent.m_sAddr.cstr (), sphSockError () );
}

/// Async resolving
using CallBack_f = std::function<void ( DWORD )>;
class DNSResolver_c
{
	struct addrinfo m_tHints { 0 };
	CallBack_f	m_pCallback			= nullptr;
	bool 		m_bCallbackInvoked	= false;
	CSphString	m_sHost;

	template <typename CALLBACK_F>
	DNSResolver_c ( const char * sHost, CALLBACK_F&& pFunc )
		: m_pCallback ( std::forward<CALLBACK_F> ( pFunc ) )
		, m_sHost ( sHost )
	{
		m_tHints.ai_family = AF_INET;
		m_tHints.ai_socktype = SOCK_STREAM;
	}

	~DNSResolver_c ()
	{
		assert ( m_pCallback );
		if ( !m_bCallbackInvoked )
			m_pCallback ( 0 );
	};

	void FinishResolve ( struct addrinfo * pResult )
	{
		assert ( m_pCallback );
		if ( !pResult )
			return;
		auto * pSockaddr_ipv4 = ( struct sockaddr_in * ) pResult->ai_addr;
		auto uIP = pSockaddr_ipv4->sin_addr.s_addr;
		freeaddrinfo ( pResult );
		m_pCallback ( uIP );
		m_bCallbackInvoked = true;
	};

	static void ResolvingRoutine ( void * pResolver )
	{
		auto * pThis = ( DNSResolver_c * ) pResolver;
		if ( pThis )
			pThis->Resolve ();
		SafeDelete ( pThis );
	}

// platform-specific part starts here.
#if HAVE_GETADDRINFO_A
	struct gaicb m_dReq, *m_pReq;

	static void ResolvingWrapper ( sigval dSigval )
	{
		ResolvingRoutine ( dSigval.sival_ptr );
	};

	void ResolveBegin ()
	{
		m_dReq = { 0 };
		m_dReq.ar_name = m_sHost.cstr ();
		m_dReq.ar_request = &m_tHints;
		m_pReq = &m_dReq;

		sigevent_t dCallBack = {{ 0 }};
		dCallBack.sigev_notify = SIGEV_THREAD;
		dCallBack.sigev_value.sival_ptr = this;
		dCallBack.sigev_notify_function = ResolvingWrapper;
		getaddrinfo_a ( GAI_NOWAIT, &m_pReq, 1, &dCallBack );
	}

	void Resolve ()
	{
		auto * pResult = m_dReq.ar_result;
		FinishResolve ( pResult );
	}
#else
	SphThread_t m_dResolverThread;

	void ResolveBegin ()
	{
		sphThreadCreate ( &m_dResolverThread, ResolvingRoutine, this, true );
	}

	void Resolve ()
	{
		struct addrinfo * pResult = nullptr;
		if ( !getaddrinfo ( m_sHost.cstr (), nullptr, &m_tHints, &pResult ) )
			FinishResolve ( pResult );
	}
#endif

public:
	//! Non-blocking DNS resolver
	//! Will call provided callback with single DWORD parameter - resolved IP. (or 0 on fail)
	//! If there is no need to go background (i.e. we have plain ip address which not need to be
	//! resolved any complex way), will call the callback immediately.
	//! \param sHost - host address, as 'www.google.com', or '127.0.0.1'
	//! \param pFunc - callback func/functor or lambda
	template <typename CALLBACK_F>
	static void GetAddress_a ( const char * sHost, CALLBACK_F && pFunc = nullptr )
	{
		if ( IsIpAddress ( sHost ) )
		{
			DWORD uResult = sphGetAddress ( sHost, false, true );
			if ( uResult )
			{
				pFunc ( uResult );
				return;
			}
		}

		auto * pPayload = new DNSResolver_c ( sHost, std::forward<CALLBACK_F>(pFunc) );
		pPayload->ResolveBegin();
	}
};

/// initialize address resolving,
/// add to agent records of stats,
/// add agent into global dashboard hash
static bool ValidateAndAddDashboard ( AgentDesc_t& dAgent, const WarnInfo_c & tInfo )
{
	assert ( !dAgent.m_pDash && !dAgent.m_pMetrics );
	if ( !ResolveAddress ( dAgent, tInfo ) )
		return false;

	Dashboard::LinkHost ( dAgent );
	dAgent.m_pMetrics = new MetricsAndCounters_t;
	assert ( dAgent.m_pDash );
	return true;
}

static IPinger* g_pPinger = nullptr;
void SetGlobalPinger ( IPinger* pPinger )
{
	g_pPinger = pPinger;
}

// Subscribe hosts with just enabled pings (i.e. where iNeedPing is exactly 1, no more)
void PingCheckAdd ( HostDashboard_t* pHost )
{
	if ( !g_pPinger )
		return;

	if ( !pHost )
		return;

	assert ( pHost->m_iNeedPing>=0 );
	if ( pHost->m_iNeedPing==1 )
		g_pPinger->Subscribe ( pHost );
}

/////////////////////////////////////////////////////////////////////////////
// MultiAgentDesc_c
//
// That is set of hosts serving one and same agent (i.e., 'mirrors').
// class also provides mirror choosing using different strategies
/////////////////////////////////////////////////////////////////////////////

GuardedHash_c & g_MultiAgents()
{
	static GuardedHash_c dGlobalHash;
	return dGlobalHash;
}

void MultiAgentDesc_c::CleanupOrphaned()
{
	// cleanup global
	auto &gAgents = g_MultiAgents ();
	bool bNeedGC = false;
	for ( WLockedHashIt_c it ( &gAgents ); it.Next (); )
	{
		auto pAgent = it.Get ();
		if ( pAgent )
		{
			pAgent->Release (); // need release since it.Get() just made AddRef().
			if ( pAgent->IsLast () )
			{
				it.Delete ();
				SafeRelease ( pAgent );
				bNeedGC = true;
			}
		}
	}
	if ( bNeedGC )
		Dashboard::CleanupOrphaned ();
}

// calculate uniq key for holding MultiAgent instance in global hash
CSphString MultiAgentDesc_c::GetKey ( const CSphVector<AgentDesc_t *> &dTemplateHosts, const AgentOptions_t &tOpt )
{
	StringBuilder_c sKey;
	for ( const auto* dHost : dTemplateHosts )
		sKey << dHost->GetMyUrl () << ":" << dHost->m_sIndexes << "|";
	sKey.Appendf ("[%d,%d,%d,%d,%d]",
		tOpt.m_bBlackhole?1:0,
		tOpt.m_bPersistent?1:0,
		(int)tOpt.m_eStrategy,
		tOpt.m_iRetryCount,
		tOpt.m_iRetryCountMultiplier);
	return sKey.cstr();
}

MultiAgentDesc_c * MultiAgentDesc_c::GetAgent ( const CSphVector<AgentDesc_t*> & dHosts, const AgentOptions_t & tOpt,
		const WarnInfo_c & tWarn ) NO_THREAD_SAFETY_ANALYSIS
{
	auto sKey = GetKey ( dHosts, tOpt );
	auto &gHash = g_MultiAgents ();

	// if an elem exists, return it addreffed.
	MultiAgentDescRefPtr_c pAgent ( ( MultiAgentDesc_c * ) gHash.Get ( sKey ) );
	if ( pAgent )
		return pAgent.Leak();

	// create and init new agent
	pAgent = new MultiAgentDesc_c;
	if ( !pAgent->Init ( dHosts, tOpt, tWarn ) )
		return nullptr;

	return ( MultiAgentDesc_c * ) gHash.TryAddThenGet ( pAgent, sKey );
}

bool MultiAgentDesc_c::Init ( const CSphVector<AgentDesc_t *> &dHosts,
			const AgentOptions_t &tOpt, const WarnInfo_c &tWarn ) NO_THREAD_SAFETY_ANALYSIS
{
	// initialize options
	m_eStrategy = tOpt.m_eStrategy;
	m_iMultiRetryCount = tOpt.m_iRetryCount * tOpt.m_iRetryCountMultiplier;
	m_sConfigStr = tWarn.m_szAgent;

	// initialize hosts & weights
	auto iLen = dHosts.GetLength ();
	Reset ( iLen );
	m_dWeights.Reset ( iLen );
	if ( !iLen )
		return tWarn.ErrSkip ( "Unable to initialize empty agent" );

	auto fFrac = 100.0f / iLen;
	ARRAY_FOREACH ( i, dHosts )
	{
		// we have templates parsed from config, NOT real working hosts!
		assert ( !dHosts[i]->m_pDash && !dHosts[i]->m_pMetrics );
		if ( !ValidateAndAddDashboard ( ( m_pData + i )->CloneFrom ( *dHosts[i] ), tWarn ) )
			return false;
		m_dWeights[i] = fFrac;
	}

	// agents with neeping!=0 will be pinged
	m_bNeedPing = IsHA () && !tOpt.m_bBlackhole;
	if ( m_bNeedPing )
		for ( int i = 0; i<GetLength (); ++i )
		{
			++m_pData[i].m_pDash->m_iNeedPing;
			PingCheckAdd ( m_pData[i].m_pDash );
		}

	return true;
}

MultiAgentDesc_c::~MultiAgentDesc_c()
{
	if ( m_bNeedPing )
		for ( int i = 0; i<GetLength (); ++i )
			--m_pData[i].m_pDash->m_iNeedPing;
}

const AgentDesc_t &MultiAgentDesc_c::RRAgent ()
{
	if ( !IsHA() )
		return *m_pData;

	auto iRRCounter = (int) m_iRRCounter++;
	while ( iRRCounter<0 || iRRCounter> ( GetLength ()-1 ) )
	{
		if ( iRRCounter+1 == (int) m_iRRCounter.CAS ( iRRCounter+1, 1 ) )
			iRRCounter = 0;
		else
			iRRCounter = (int) m_iRRCounter++;
	}

	return m_pData[iRRCounter];
}

const AgentDesc_t &MultiAgentDesc_c::RandAgent ()
{
	return m_pData[sphRand () % GetLength ()];
}

void MultiAgentDesc_c::ChooseWeightedRandAgent ( int * pBestAgent, CSphVector<int> & dCandidates )
{
	assert ( pBestAgent );
	CSphScopedRLock tLock ( m_dWeightLock );
	auto fBound = m_dWeights[*pBestAgent];
	auto fLimit = fBound;
	for ( auto j : dCandidates )
		fLimit += m_dWeights[j];
	auto fChance = sphRand () * fLimit / UINT_MAX;

	if ( fChance<=fBound )
		return;

	for ( auto j : dCandidates )
	{
		fBound += m_dWeights[j];
		*pBestAgent = j;
		if ( fChance<=fBound )
			break;
	}
}

static void LogAgentWeights ( const float * pOldWeights, const float * pCurWeights, const int64_t * pTimers, const CSphFixedVector<AgentDesc_t> & dAgents )
{
	ARRAY_FOREACH ( i, dAgents )
		sphLogDebug ( "client=%s, mirror=%d, weight=%0.2f%%, %0.2f%%, timer=" INT64_FMT
					  , dAgents[i].GetMyUrl ().cstr (), i, pCurWeights[i], pOldWeights[i], pTimers[i] );
}

const AgentDesc_t &MultiAgentDesc_c::StDiscardDead ()
{
	if ( !IsHA() )
		return m_pData[0];

	// threshold errors-a-row to be counted as dead
	int iDeadThr = 3;

	int iBestAgent = -1;
	int64_t iErrARow = -1;
	CSphVector<int> dCandidates;
	CSphFixedVector<int64_t> dTimers ( GetLength() );
	dCandidates.Reserve ( GetLength() );

	for (int i=0; i<GetLength(); ++i)
	{
		// no locks for g_pStats since we just reading, and read data is not critical.
		const HostDashboard_t * pDash = m_pData[i].m_pDash;

		HostMetricsSnapshot_t dMetricsSnapshot;
		pDash->GetCollectedMetrics ( dMetricsSnapshot );// look at last 30..90 seconds.
		uint64_t uQueries = 0;
		for ( int j=0; j<eMaxAgentStat; ++j )
			uQueries += dMetricsSnapshot[j];
		if ( uQueries > 0 )
			dTimers[i] = dMetricsSnapshot[ehTotalMsecs]/uQueries;
		else
			dTimers[i] = 0;

		CSphScopedRLock tRguard ( pDash->m_dMetricsLock );
		int64_t iThisErrARow = ( pDash->m_iErrorsARow<=iDeadThr ) ? 0 : pDash->m_iErrorsARow;

		if ( iErrARow < 0 )
			iErrARow = iThisErrARow;

		// 2. Among good nodes - select the one(s) with lowest errors/query rating
		if ( iErrARow > iThisErrARow )
		{
			dCandidates.Reset();
			iBestAgent = i;
			iErrARow = iThisErrARow;
		} else if ( iErrARow==iThisErrARow )
		{
			if ( iBestAgent>=0 )
				dCandidates.Add ( iBestAgent );
			iBestAgent = i;
		}
	}

	// check if it is a time to recalculate the agent's weights
	CheckRecalculateWeights ( dTimers );


	// nothing to select, sorry. Just random agent...
	if ( iBestAgent < 0 )
	{
		sphLogDebug ( "HA selector discarded all the candidates and just fall into simple Random" );
		return RandAgent();
	}

	// only one node with lowest error rating. Return it.
	if ( !dCandidates.GetLength() )
	{
		sphLogDebug ( "client=%s, HA selected %d node with best num of errors a row (" INT64_FMT ")"
					  , m_pData[iBestAgent].GetMyUrl().cstr(), iBestAgent, iErrARow );
		return m_pData[iBestAgent];
	}

	// several nodes. Let's select the one.
	ChooseWeightedRandAgent ( &iBestAgent, dCandidates );
	if ( g_eLogLevel>=SPH_LOG_VERBOSE_DEBUG )
	{
		float fAge = 0.0;
		const HostDashboard_t & dDash = *m_pData[iBestAgent].m_pDash;
		CSphScopedRLock tRguard ( dDash.m_dMetricsLock );
		fAge = ( dDash.m_iLastAnswerTime-dDash.m_iLastQueryTime ) / 1000.0f;
		sphLogDebugv ("client=%s, HA selected %d node by weighted random, with best EaR ("
						  INT64_FMT "), last answered in %.3f milliseconds, among %d candidates"
					  , m_pData[iBestAgent].GetMyUrl ().cstr(), iBestAgent, iErrARow, fAge, dCandidates.GetLength()+1 );
	}

	return m_pData[iBestAgent];
}

// Check the time and recalculate mirror weights, if necessary.
void MultiAgentDesc_c::CheckRecalculateWeights ( const CSphFixedVector<int64_t> & dTimers )
{
	if ( !dTimers.GetLength () || !HostDashboard_t::IsHalfPeriodChanged ( &m_uTimestamp ) )
		return;

	CSphFixedVector<float> dWeights {0};

	// since we'll update values anyway, acquire w-lock.
	CSphScopedWLock tWguard ( m_dWeightLock );
	dWeights.CopyFrom ( m_dWeights );
	RebalanceWeights ( dTimers, dWeights );
	if ( g_eLogLevel>=SPH_LOG_DEBUG )
		LogAgentWeights ( m_dWeights.Begin(), dWeights.Begin (), dTimers.Begin (), *this );
	m_dWeights.SwapData (dWeights);
}

const AgentDesc_t &MultiAgentDesc_c::StLowErrors ()
{
	if ( !IsHA() )
		return *m_pData;

	// how much error rating is allowed
	float fAllowedErrorRating = 0.03f; // i.e. 3 errors per 100 queries is still ok

	int iBestAgent = -1;
	float fBestCriticalErrors = 1.0;
	float fBestAllErrors = 1.0;
	CSphVector<int> dCandidates;
	CSphFixedVector<int64_t> dTimers ( GetLength() );
	dCandidates.Reserve ( GetLength() );

	for ( int i=0; i<GetLength(); ++i )
	{
		// no locks for g_pStats since we just reading, and read data is not critical.
		const HostDashboard_t & dDash = *m_pData[i].m_pDash;

		HostMetricsSnapshot_t dMetricsSnapshot;
		dDash.GetCollectedMetrics ( dMetricsSnapshot ); // look at last 30..90 seconds.
		uint64_t uQueries = 0;
		uint64_t uCriticalErrors = 0;
		uint64_t uAllErrors = 0;
		uint64_t uSuccesses = 0;
		for ( int j=0; j<eMaxAgentStat; ++j )
		{
			if ( j==eNetworkCritical )
				uCriticalErrors = uQueries;
			else if ( j==eNetworkNonCritical )
			{
				uAllErrors = uQueries;
				uSuccesses = dMetricsSnapshot[j];
			}
			uQueries += dMetricsSnapshot[j];
		}

		if ( uQueries > 0 )
			dTimers[i] = dMetricsSnapshot[ehTotalMsecs]/uQueries;
		else
			dTimers[i] = 0;

		// 1. No successes queries last period (it includes the pings). Skip such node!
		if ( !uSuccesses )
			continue;

		if ( uQueries )
		{
			// 2. Among good nodes - select the one(s) with lowest errors/query rating
			float fCriticalErrors = (float) uCriticalErrors/uQueries;
			float fAllErrors = (float) uAllErrors/uQueries;
			if ( fCriticalErrors<=fAllowedErrorRating )
				fCriticalErrors = 0.0f;
			if ( fAllErrors<=fAllowedErrorRating )
				fAllErrors = 0.0f;
			if ( fCriticalErrors < fBestCriticalErrors )
			{
				dCandidates.Reset();
				iBestAgent = i;
				fBestCriticalErrors = fCriticalErrors;
				fBestAllErrors = fAllErrors;
			} else if ( fCriticalErrors==fBestCriticalErrors )
			{
				if ( fAllErrors < fBestAllErrors )
				{
					dCandidates.Reset();
					iBestAgent = i;
					fBestAllErrors = fAllErrors;
				} else if ( fAllErrors==fBestAllErrors )
				{
					if ( iBestAgent>=0 )
						dCandidates.Add ( iBestAgent );
					iBestAgent = i;
				}
			}
		}
	}

	// check if it is a time to recalculate the agent's weights
	CheckRecalculateWeights ( dTimers );

	// nothing to select, sorry. Just plain RR...
	if ( iBestAgent < 0 )
	{
		sphLogDebug ( "HA selector discarded all the candidates and just fall into simple Random" );
		return RandAgent();
	}

	// only one node with lowest error rating. Return it.
	if ( !dCandidates.GetLength() )
	{
		sphLogDebug ( "client=%s, HA selected %d node with best error rating (%.2f)",
			m_pData[iBestAgent].GetMyUrl().cstr(), iBestAgent, fBestCriticalErrors );
		return m_pData[iBestAgent];
	}

	// several nodes. Let's select the one.
	ChooseWeightedRandAgent ( &iBestAgent, dCandidates );
	if ( g_eLogLevel>=SPH_LOG_VERBOSE_DEBUG )
	{
		float fAge = 0.0f;
		const HostDashboard_t & dDash = *m_pData[iBestAgent].m_pDash;
		CSphScopedRLock tRguard ( dDash.m_dMetricsLock );
		fAge = ( dDash.m_iLastAnswerTime-dDash.m_iLastQueryTime ) / 1000.0f;
		sphLogDebugv (
			"client=%s, HA selected %d node by weighted random, "
				"with best error rating (%.2f), answered %f seconds ago"
			, m_pData[iBestAgent].GetMyUrl ().cstr(), iBestAgent, fBestCriticalErrors, fAge );
	}

	return m_pData[iBestAgent];
}


const AgentDesc_t &MultiAgentDesc_c::ChooseAgent ()
{
	if ( !IsHA() )
	{
		assert ( m_pData && "Not initialized MultiAgent detected!");
		if ( m_pData )
			return *m_pData;

		static AgentDesc_t dFakeHost; // avoid crash in release if not initialized.
		return dFakeHost;
	}

	switch ( m_eStrategy )
	{
	case HA_AVOIDDEAD:
		return StDiscardDead();
	case HA_AVOIDERRORS:
		return StLowErrors();
	case HA_ROUNDROBIN:
		return RRAgent();
	default:
		return RandAgent();
	}
}

const char * Agent_e_Name ( Agent_e eState )
{
	switch ( eState )
	{
	case Agent_e::HEALTHY: return "HEALTHY";
	case Agent_e::CONNECTING: return "CONNECTING";
	case Agent_e::RETRY: return "RETRY";
	}
	assert ( 0 && "UNKNOWN_STATE" );
	return "UNKNOWN_STATE";
}

SearchdStats_t g_tStats;

// generic stats track - always to agent stats, separately to dashboard.
void agent_stats_inc ( AgentConn_t &tAgent, AgentStats_e iCountID )
{
	assert ( iCountID<=eMaxAgentStat );
	assert ( tAgent.m_tDesc.m_pDash );

	if ( tAgent.m_tDesc.m_pMetrics )
		++tAgent.m_tDesc.m_pMetrics->m_dCounters[iCountID];

	HostDashboard_t &tIndexDash = *tAgent.m_tDesc.m_pDash;
	CSphScopedWLock tWguard ( tIndexDash.m_dMetricsLock );
	MetricsAndCounters_t &tAgentMetrics = tIndexDash.GetCurrentMetrics ();
	tAgentMetrics.m_dCounters[iCountID]++;
	if ( iCountID>=eNetworkNonCritical && iCountID<eMaxAgentStat )
		tIndexDash.m_iErrorsARow = 0;
	else
		tIndexDash.m_iErrorsARow++;

	tAgent.m_iEndQuery = sphMicroTimer ();
	tIndexDash.m_iLastQueryTime = tAgent.m_iStartQuery;
	tIndexDash.m_iLastAnswerTime = tAgent.m_iEndQuery;

	// do not count query time for unlinked connections (pings)
	// only count errors
	if ( tAgent.m_tDesc.m_pMetrics )
	{
		tAgentMetrics.m_dMetrics[ehTotalMsecs] += tAgent.m_iEndQuery - tAgent.m_iStartQuery;
		tAgent.m_tDesc.m_pMetrics->m_dMetrics[ehTotalMsecs] += tAgent.m_iEndQuery - tAgent.m_iStartQuery;
	}
}

// special case of stats - all is ok, just need to track the time in dashboard.
void track_processing_time ( AgentConn_t &tAgent )
{
	// first we count temporary statistic (into dashboard)
	assert ( tAgent.m_tDesc.m_pDash );
	uint64_t uConnTime = ( uint64_t ) sphMicroTimer () - tAgent.m_iStartQuery;
	{
		CSphScopedWLock tWguard ( tAgent.m_tDesc.m_pDash->m_dMetricsLock );
		uint64_t * pMetrics = tAgent.m_tDesc.m_pDash->GetCurrentMetrics ().m_dMetrics;

		++pMetrics[ehConnTries];
		if ( uint64_t ( uConnTime )>pMetrics[ehMaxMsecs] )
			pMetrics[ehMaxMsecs] = uConnTime;

		if ( pMetrics[ehConnTries]>1 )
			pMetrics[ehAverageMsecs] =
				( pMetrics[ehAverageMsecs] * ( pMetrics[ehConnTries] - 1 ) + uConnTime ) / pMetrics[ehConnTries];
		else
			pMetrics[ehAverageMsecs] = uConnTime;
	} // no need to hold dashboard anymore


	if ( !tAgent.m_tDesc.m_pMetrics )
		return;

	// then we count permanent statistic (for show status)
	uint64_t * pHStat = tAgent.m_tDesc.m_pMetrics->m_dMetrics;
	++pHStat[ehConnTries];
	if ( uint64_t ( uConnTime )>pHStat[ehMaxMsecs] )
		pHStat[ehMaxMsecs] = uConnTime;
	if ( pHStat[ehConnTries]>1 )
		pHStat[ehAverageMsecs] =
			( pHStat[ehAverageMsecs] * ( pHStat[ehConnTries] - 1 ) + uConnTime ) / pHStat[ehConnTries];
	else
		pHStat[ehAverageMsecs] = uConnTime;
}

/// try to parse hostname/ip/port or unixsocket on current pConfigLine.
/// fill pAgent fields on success and move ppLine pointer next after parsed instance
/// test cases and test group 'T_ParseAddressPort', are in gtest_searchdaemon.cpp.
bool ParseAddressPort ( HostDesc_t& dHost, const char ** ppLine, const WarnInfo_c & tInfo )
{
	// extract host name or path
	const char *&p = *ppLine;
	const char * pAnchor = p;

	if ( !p )
		return false;

	enum AddressType_e { apIP, apUNIX };
	AddressType_e eState = apIP;
	if ( *p=='/' ) // whether we parse inet or unix socket
		eState = apUNIX;

	while ( sphIsAlpha ( *p ) || *p=='.' || *p=='-' || *p=='/' )
		++p;
	if ( p==pAnchor )
		return tInfo.ErrSkip ( "host name or path expected" );

	CSphString sSub ( pAnchor, p-pAnchor );
	if ( eState==apUNIX )
	{
#if !USE_WINDOWS
		if ( strlen ( sSub.cstr () ) + 1>sizeof(((struct sockaddr_un *)0)->sun_path) )
			return tInfo.ErrSkip ( "UNIX socket path is too long" );
#endif
		dHost.m_iFamily = AF_UNIX;
		dHost.m_sAddr = sSub;
		return true;
	}

	// below is only deal with inet sockets
	dHost.m_iFamily = AF_INET;
	dHost.m_sAddr = sSub;

	// expect ':' (and then portnum) after address
	if ( *p!=':' )
	{
		dHost.m_iPort = IANA_PORT_SPHINXAPI;
		tInfo.Warn ( "colon and portnum expected before '%s' - Using default IANA %d port", p, dHost.m_iPort );
		return true;
	}
	pAnchor = ++p;

	// parse portnum
	while ( isdigit(*p) )
		++p;

	if ( p==pAnchor )
	{
		dHost.m_iPort = IANA_PORT_SPHINXAPI;
		tInfo.Warn ( "portnum expected before '%s' - Using default IANA %d port", p, dHost.m_iPort );
		--p; /// step back to ':'
		return true;
	}
	dHost.m_iPort = atoi ( pAnchor );

	if ( !IsPortInRange ( dHost.m_iPort ) )
		return tInfo.ErrSkip ( "invalid port number near '%s'", p );

	return true;
}

bool ParseStrategyHA ( const char * sName, HAStrategies_e & eStrategy )
{
	if ( sphStrMatchStatic ( "random", sName ) )
		eStrategy = HA_RANDOM;
	else if ( sphStrMatchStatic ( "roundrobin", sName ) )
		eStrategy = HA_ROUNDROBIN;
	else if ( sphStrMatchStatic ( "nodeads", sName ) )
		eStrategy = HA_AVOIDDEAD;
	else if ( sphStrMatchStatic ( "noerrors", sName ) )
		eStrategy = HA_AVOIDERRORS;
	else
		return false;

	return true;
}

CSphString HAStrategyToStr ( HAStrategies_e eStrategy )
{
	switch ( eStrategy )
	{
	case HA_RANDOM:			return "random";
	case HA_ROUNDROBIN:		return "roundrobin";
	case HA_AVOIDDEAD:		return "nodeads";
	case HA_AVOIDERRORS:	return "noerrors";
	}

	return "";
}

void ParseIndexList ( const CSphString &sIndexes, StrVec_t &dOut )
{
	CSphString sSplit = sIndexes;
	if ( sIndexes.IsEmpty () )
		return;

	auto * p = ( char * ) sSplit.cstr ();
	while ( *p )
	{
		// skip non-alphas
		while ( *p && !isalpha ( *p ) && !isdigit ( *p ) && *p!='_' )
			p++;
		if ( !( *p ) )
			break;

		// FIXME?
		// We no not check that index name shouldn't start with '_'.
		// That means it's de facto allowed for API queries.
		// But not for SphinxQL ones.

		// this is my next index name
		const char * sNext = p;
		while ( isalpha ( *p ) || isdigit ( *p ) || *p=='_' )
			p++;

		assert ( sNext!=p );
		if ( *p )
			*p++ = '\0'; // if it was not the end yet, we'll continue from next char

		dOut.Add ( sNext );
	}
}

// parse agent's options line and modify pOptions
bool ParseOptions ( AgentOptions_t * pOptions, const CSphString& sOptions, const WarnInfo_c & tWI )
{
	StrVec_t dSplitParts;
	sphSplit ( dSplitParts, sOptions.cstr (), "," ); // diff. options are ,-separated
	for ( auto &sOption : dSplitParts )
	{
		if ( sOption.IsEmpty () )
			continue;

		// split '=' separated pair into tokens
		StrVec_t dOption;
		sphSplit ( dOption, sOption.cstr (), "=" );
		if ( dOption.GetLength ()!=2 )
			return tWI.ErrSkip ( "option %s error: option and value must be =-separated pair", sOption.cstr () );

		for ( auto &sOpt : dOption )
			sOpt.ToLower ().Trim ();

		const char * sOptName = dOption[0].cstr ();
		const char * sOptValue = dOption[1].cstr ();
		if ( sphStrMatchStatic ( "conn", sOptName ) )
		{
			if ( sphStrMatchStatic ( "pconn", sOptValue ) || sphStrMatchStatic ( "persistent", sOptValue ) )
			{
				pOptions->m_bPersistent = true;
				continue;
			}
		} else if ( sphStrMatchStatic ( "ha_strategy", sOptName ) )
		{
			if ( ParseStrategyHA ( sOptValue, pOptions->m_eStrategy ) )
				continue;
		} else if ( sphStrMatchStatic ( "blackhole", sOptName ) )
		{
			pOptions->m_bBlackhole = ( atoi ( sOptValue )!=0 );
			continue;
		} else if ( sphStrMatchStatic ( "retry_count", sOptName ) )
		{
			pOptions->m_iRetryCount = atoi ( sOptValue );
			pOptions->m_iRetryCountMultiplier = 1;
			continue;
		}
		return tWI.ErrSkip ( "unknown agent option '%s'", sOption.cstr () );
	}
	return true;
}

// check whether all index(es) in list are valid index names
bool CheckIndexNames ( const CSphString &sIndexes, const WarnInfo_c & tWI )
{
	StrVec_t dRawIndexes, dParsedIndexes;

	// compare two lists: one made by raw splitting with ',' character,
	// second made by our ParseIndexList function.
	sphSplit(dRawIndexes, sIndexes.cstr(), ",");
	ParseIndexList ( sIndexes, dParsedIndexes );

	if ( dParsedIndexes.GetLength ()==dRawIndexes.GetLength () )
		return true;

	ARRAY_FOREACH( i, dParsedIndexes )
	{
		dRawIndexes[i].Trim();
		if ( dRawIndexes[i]!= dParsedIndexes[i] )
			return tWI.ErrSkip ("no such index: %s", dRawIndexes[i].cstr());
	}
	return true;
}

// parse agent string into template vec of AgentDesc_t, then configure them.
static bool ConfigureMirrorSet ( CSphVector<AgentDesc_t*> &tMirrors, AgentOptions_t * pOptions, const WarnInfo_c & tWI )
{
	assert ( tMirrors.IsEmpty () );

	StrVec_t dSplitParts;
	sphSplit ( dSplitParts, tWI.m_szAgent, "[]" );
	if ( dSplitParts.IsEmpty () )
		return tWI.ErrSkip ( "empty agent definition" );

	if ( dSplitParts[0].IsEmpty () )
		return tWI.ErrSkip ( "one or more hosts/sockets expected before [" );

	if ( dSplitParts.GetLength ()<1 || dSplitParts.GetLength ()>2 )
		return tWI.ErrSkip ( "wrong syntax: expected one or more hosts/sockets, then m.b. []-enclosed options" );

	// parse agents
	// separate '|'-splitted records, normalize result
	StrVec_t dRawAgents;
	sphSplit ( dRawAgents, dSplitParts[0].cstr (), "|" );
	for ( auto &sAgent : dRawAgents )
		sAgent.Trim ();

	if ( dSplitParts.GetLength ()==2 && !ParseOptions ( pOptions, dSplitParts[1], tWI ) )
		return false;

	assert ( dRawAgents.GetLength ()>0 );

	// parse separate strings into agent descriptors
	for ( auto &sAgent: dRawAgents )
	{
		if ( sAgent.IsEmpty () )
			continue;

		tMirrors.Add ( new AgentDesc_t );
		AgentDesc_t &dMirror = *tMirrors.Last ();
		const char * sRawAgent = sAgent.cstr ();
		if ( !ParseAddressPort ( dMirror, &sRawAgent, tWI ) )
			return false;

		// apply per-mirror options
		dMirror.m_bPersistent = pOptions->m_bPersistent;
		dMirror.m_bBlackhole = pOptions->m_bBlackhole;

		if ( *sRawAgent )
		{
			if ( *sRawAgent!=':' )
				return tWI.ErrSkip ( "after host/socket expected ':', then index(es), but got '%s')", sRawAgent );

			CSphString sIndexList = ++sRawAgent;
			sIndexList.Trim ();
			if ( sIndexList.IsEmpty () )
				continue;

			if ( !CheckIndexNames ( sIndexList, tWI ) )
				return false;

			dMirror.m_sIndexes = sIndexList;
		}
	}

	// fixup multiplier (if it is 0, it is still not defined).
	if ( !pOptions->m_iRetryCountMultiplier )
		pOptions->m_iRetryCountMultiplier = tMirrors.GetLength ();

	// fixup agent's indexes name: if no index assigned, stick the next one (or the parent).
	CSphString sLastIndex = tWI.m_szIndexName;
	for ( int i = tMirrors.GetLength () - 1; i>=0; --i )
		if ( tMirrors[i]->m_sIndexes.IsEmpty () )
			tMirrors[i]->m_sIndexes = sLastIndex;
		else
			sLastIndex = tMirrors[i]->m_sIndexes;

	return true;
}

// different cases are tested in T_ConfigureMultiAgent, see gtests_searchdaemon.cpp
MultiAgentDesc_c * ConfigureMultiAgent ( const char * szAgent, const char * szIndexName, AgentOptions_t tOptions, StrVec_t * pWarnings )
{
	CSphVector<AgentDesc_t *> tMirrors;
	auto dFree = AtScopeExit ( [&tMirrors] { tMirrors.Apply( [] ( AgentDesc_t * pMirror ) { SafeDelete ( pMirror ); } ); } );

	WarnInfo_c tWI ( szIndexName, szAgent, pWarnings );

	if ( !ConfigureMirrorSet ( tMirrors, &tOptions, tWI ) )
		return nullptr;

	return MultiAgentDesc_c::GetAgent ( tMirrors, tOptions, tWI );
}

HostDesc_t &HostDesc_t::CloneFromHost ( const HostDesc_t &rhs )
{
	if ( &rhs==this )
		return *this;
	m_pDash = rhs.m_pDash;
	m_bBlackhole = rhs.m_bBlackhole;
	m_uAddr = rhs.m_uAddr;
	m_bNeedResolve = rhs.m_bNeedResolve;
	m_bPersistent = rhs.m_bPersistent;
	m_iFamily = rhs.m_iFamily;
	m_sAddr = rhs.m_sAddr;
	m_iPort = rhs.m_iPort;
	return *this;
}

AgentDesc_t &AgentDesc_t::CloneFrom ( const AgentDesc_t &rhs )
{
	if ( &rhs==this )
		return *this;
	CloneFromHost ( rhs );
	m_sIndexes = rhs.m_sIndexes;
	m_pMetrics = rhs.m_pMetrics;
	return *this;
}

//// global dashboard

namespace Dashboard {
	RwLock_t g_tDashLock;
	VecRefPtrs_t<HostDashboard_t*> g_dDashes GUARDED_BY( g_tDashLock );

void CleanupOrphaned ()
{
	CSphScopedWLock tWguard ( g_tDashLock );
	ARRAY_FOREACH ( i, g_dDashes )
	{
		auto pDash = g_dDashes[i];
		if ( pDash->IsLast () )
		{
			g_dDashes.RemoveFast ( i-- ); // remove, and then step back
			SafeRelease ( pDash );
		}
	}
}

// Due to very rare template of usage, linear search is quite enough here
HostDashboardRefPtr_t FindAgent ( const CSphString& sAgent )
	{
		CSphScopedRLock tRguard ( g_tDashLock );
		for ( auto* pDash : g_dDashes )
		{
			if ( pDash->IsLast ())
				continue;

			if ( pDash->m_tHost.GetMyUrl ()==sAgent )
			{
				pDash->AddRef ();
				return HostDashboardRefPtr_t ( pDash );
			}
		}
		return HostDashboardRefPtr_t (); // not found
	}

void LinkHost ( HostDesc_t &dHost )
{
	assert ( !dHost.m_pDash );
	dHost.m_pDash = FindAgent ( dHost.GetMyUrl() );
	if ( dHost.m_pDash )
		return;

	// nothing found existing; so create the new.
	dHost.m_pDash = new HostDashboard_t ( dHost );
	CSphScopedWLock tWguard ( g_tDashLock );
	g_dDashes.Add ( dHost.m_pDash );
	dHost.m_pDash->AddRef(); // one link here in vec, other returned with the host
}

VecRefPtrs_t<HostDashboard_t*> GetActiveHosts ()
{
	VecRefPtrs_t<HostDashboard_t*> dAgents;
	assert ( dAgents.IsEmpty ());
	ScRL_t tRguard ( g_tDashLock );
	for ( auto * pDash : g_dDashes )
	{
		if ( pDash->IsLast() )
			continue;

		pDash->AddRef ();
		dAgents.Add ( pDash );
	}
	return dAgents;
}
} // namespace Dashboard


/// IOVec_c : wrapper over vector of system iovec/WSABuf
/////////////////////////////////////////////////////////////////////////////
void IOVec_c::BuildFrom ( const SmartOutputBuffer_t &tSource )
{
	tSource.GetIOVec ( m_dIOVec );
	if ( m_dIOVec.IsEmpty () )
		return;
	m_iIOChunks = ( size_t ) m_dIOVec.GetLength ();
}

void IOVec_c::Reset()
{
	m_dIOVec.Reset();
	m_iIOChunks = 0;
}

void IOVec_c::StepForward ( size_t uStep )
{
	auto iLen = m_dIOVec.GetLength ();
	for ( ; m_iIOChunks>0; --m_iIOChunks )
	{
		auto &dIOVec = m_dIOVec[iLen - m_iIOChunks];
		if ( uStep<IOLEN( dIOVec ) )
		{
			IOPTR ( dIOVec ) = IOBUFTYPE ( ( BYTE * ) IOPTR ( dIOVec ) + uStep );
			IOLEN( dIOVec ) -= uStep;
			break;
		}
		uStep -= IOLEN ( dIOVec );
	}
}

/// PollableEvent_c : an event which could be watched by poll/epoll/kqueue
/////////////////////////////////////////////////////////////////////////////

void SafeCloseSocket ( int & iFD )
{
	if ( iFD>=0 )
		sphSockClose ( iFD );
	iFD = -1;
}

#if !HAVE_EVENTFD
static bool CreateSocketPair ( int &iSock1, int &iSock2, CSphString &sError )
{
#if USE_WINDOWS
	union {
		struct sockaddr_in inaddr;
		struct sockaddr addr;
	} tAddr;

	int iListen = socket ( AF_INET, SOCK_STREAM, IPPROTO_TCP );
	if ( iListen<0 )
	{
		sError.SetSprintf ( "failed to create listen socket: %s", sphSockError() );
		return false;
	}

	memset ( &tAddr, 0, sizeof ( tAddr ) );
	tAddr.inaddr.sin_family = AF_INET;
	tAddr.inaddr.sin_addr.s_addr = htonl ( INADDR_LOOPBACK );
	tAddr.inaddr.sin_port = 0;
	auto tCloseListen = AtScopeExit  ( [&iListen] { if ( iListen>=0 ) sphSockClose (iListen); } );

	if ( bind ( iListen, &tAddr.addr, sizeof ( tAddr.inaddr ) )<0 )
	{
		sError.SetSprintf ( "failed to bind listen socket: %s", sphSockError() );
		return false;
	}

	int iAddrBufLen = sizeof ( tAddr );
	memset ( &tAddr, 0, sizeof ( tAddr ) );
	if ( getsockname ( iListen, &tAddr.addr, &iAddrBufLen )<0 )
	{
		sError.SetSprintf ( "failed to get socket description: %s", sphSockError() );
		return false;
	}

	tAddr.inaddr.sin_addr.s_addr = htonl ( INADDR_LOOPBACK );
	tAddr.inaddr.sin_family = AF_INET;

	if ( listen ( iListen, 5 )<0 )
	{
		sError.SetSprintf ( "failed to listen socket: %s", sphSockError() );
		return false;
	}

	int iWrite = socket ( AF_INET, SOCK_STREAM, 0 );
	auto tCloseWrite = AtScopeExit  ( [&iWrite] { if ( iWrite>=0 ) sphSockClose (iWrite); } );

	if ( iWrite<0 )
	{
		sError.SetSprintf ( "failed to create write socket: %s", sphSockError() );
		return false;
	}

	if ( connect ( iWrite, &tAddr.addr, sizeof(tAddr.addr) )<0 )
	{
		sError.SetSprintf ( "failed to connect to loopback: %s\n", sphSockError() );
		return false;
	}

	int iRead = accept ( iListen, NULL, NULL );
	if ( iRead<0 )
	{
		sError.SetSprintf ( "failed to accept loopback: %s\n", sphSockError() );
	}

	iSock1 = iRead;
	iSock2 = iWrite;
	iWrite = -1; // protect from tCloseWrite

#else
	int dSockets[2] = { -1, -1 };
	if ( socketpair ( AF_LOCAL, SOCK_STREAM, 0, dSockets )!=0 )
	{
		sError.SetSprintf ( "failed to create socketpair: %s", sphSockError () );
		return false;
	}

	iSock1 = dSockets[0];
	iSock2 = dSockets[1];

#endif

	if ( sphSetSockNB ( iSock1 )<0 || sphSetSockNB ( iSock2 )<0 )
	{
		sError.SetSprintf ( "failed to set socket non-block: %s", sphSockError () );
		SafeCloseSocket ( iSock1 );
		SafeCloseSocket ( iSock2 );
		return false;
	}

#ifdef TCP_NODELAY
	int iOn = 1;
	if ( setsockopt ( iSock2, IPPROTO_TCP, TCP_NODELAY, (char*)&iOn, sizeof ( iOn ) )<0 )
		sphWarning ( "failed to set nodelay option: %s", sphSockError() );
#endif
	return true;
}
#endif


inline static bool IS_PENDING ( int iErr )
{
#if USE_WINDOWS
	return iErr==ERROR_IO_PENDING || iErr==0;
#else
	return iErr==EAGAIN || iErr==EWOULDBLOCK;
#endif
}

inline static bool IS_PENDING_PROGRESS ( int iErr )
{
	return IS_PENDING ( iErr ) || iErr==EINPROGRESS;
}

/////////////////////////////////////////////////////////////////////////////
// some extended win-specific stuff
#if USE_WINDOWS

static LPFN_CONNECTEX ConnectEx = nullptr;

#ifndef WSAID_CONNECTEX
#define WSAID_CONNECTEX \
	{0x25a207b9,0xddf3,0x4660,{0x8e,0xe9,0x76,0xe5,0x8c,0x74,0x06,0x3e}}
#endif

bool LoadExFunctions ()
{
	GUID uGuidConnectX = WSAID_CONNECTEX;

	// Dummy socket (for WSAIoctl call)
	SOCKET iSocket = socket ( AF_INET, SOCK_STREAM, 0 );
	if ( iSocket == INVALID_SOCKET )
		return false;

	// fill addr of ConnectX function
	DWORD	m_Bytes;
	auto iRes = WSAIoctl ( iSocket, SIO_GET_EXTENSION_FUNCTION_POINTER,
		&uGuidConnectX, sizeof ( uGuidConnectX ),
		&ConnectEx, sizeof ( ConnectEx ),
		&m_Bytes, NULL, NULL );

	closesocket ( iSocket );
	return ( iRes==0 );
}
#endif

/////////////////////////////////////////////////////////////////////////////
/// AgentConn_t
///
/// Network task: socket, io buffers, query/result parsers,
/// helper structs.
/////////////////////////////////////////////////////////////////////////////
AgentConn_t::~AgentConn_t ()
{
	sphLogDebugv ( "AgentConn %p destroyed", this );
	if ( m_iSock>=0 )
		Finish ();
}

void AgentConn_t::State ( Agent_e eState )
{
	sphLogDebugv ( "state %s > %s, sock %d, order %d, %p",
		Agent_e_Name ( m_eConnState ), Agent_e_Name ( eState ), m_iSock, m_iStoreTag, this );
	m_eConnState = eState;
}


// initialize socket from persistent pool (it m.b. disconnected or not initialized, however).
bool AgentConn_t::IsPersistent ()
{
	return m_tDesc.m_bPersistent && m_tDesc.m_pDash && m_tDesc.m_pDash->m_pPersPool;
}



// return socket to the pool (it m.b. connected!)
void AgentConn_t::ReturnPersist ()
{
	assert ( ( m_iSock==-1 ) || IsPersistent () ); // otherwize it will leak...
	if ( IsPersistent() )
		m_tDesc.m_pDash->m_pPersPool->ReturnConnection ( m_iSock );
	m_iSock = -1;
}

/// 'soft' failure - log and return false
bool AgentConn_t::Fail ( const char * sFmt, ... )
{
	va_list ap;
	va_start ( ap, sFmt );
	m_sFailure.SetSprintfVa ( sFmt, ap );
	va_end ( ap );
	sphLogDebugA ( "%d Fail() %s, ref=%d", m_iStoreTag, m_sFailure.cstr (), ( int ) GetRefcount () );
	return false;
}

//! 'hard' failure - close socket, switch to 'RETRY' state, then write stats, log and return false
//! \param eStat - error category
bool AgentConn_t::Fatal ( AgentStats_e eStat, const char * sMessage, ... )
{
	va_list ap;
	va_start ( ap, sMessage );
	m_sFailure.SetSprintfVa ( sMessage, ap );
	va_end ( ap );
	sphLogDebugA ( "%d FATAL: %s", m_iStoreTag, m_sFailure.cstr () );
	State ( Agent_e::RETRY );
	Finish ( true );
	agent_stats_inc ( *this, eStat );
	return false;
}

/// correct way to close connection:
void AgentConn_t::Finish ( bool bFail )
{
	if ( m_iSock>=0 && ( bFail || !IsPersistent() ) )
	{
		sphLogDebugA ( "%d Socket %d closed and turned to -1", m_iStoreTag, m_iSock );
		SafeCloseSocket ( m_iSock );
	}

	sphLogDebugA ( "%d Abort all callbacks ref=%d", m_iStoreTag, ( int ) GetRefcount () );
	LazyDeleteOrChange (); // remove timer and all callbacks, if any
	m_pPollerTask = nullptr;

	ReturnPersist ();
	if ( m_iStartQuery )
		m_iWall += sphMicroTimer () - m_iStartQuery; // imitated old behaviour
}

//! Failure from successfully ended session
//! (i.e. no network issues, but error in packet itself - like bad syntax, or simple 'try again').
//! so, we don't have to corrupt agent's stat in the case.
bool AgentConn_t::BadResult ( int iError )
{
	sphLogDebugA ( "%d BadResult() ref=%d", m_iStoreTag, ( int ) GetRefcount () );
	if ( iError==-1 )
		sphLogDebugA ( "%d text error is %s", m_iStoreTag, m_sFailure.cstr () );
	else if ( iError )
		sphLogDebugA ( "%d error is %d, %s", m_iStoreTag, iError, sphSockError ( iError ) );

	State ( Agent_e::RETRY );
	Finish ();
	if ( m_pResult )
		m_pResult->Reset ();
	return false;
}

void AgentConn_t::ReportFinish ( bool bSuccess )
{
	if ( m_pReporter )
		m_pReporter->Report ( bSuccess );
	m_iRetries = -1; // avoid any accidental retry in future. fixme! better investigate why such accident may happen
	m_bManyTries = false; // avoid report message because of it.
}

/// switch from 'connecting' to 'healthy' state.
/// track the time, modify timeout from 'connect' to 'query', inform poller about it.
void AgentConn_t::SendingState ()
{
	if ( StateIs ( Agent_e::CONNECTING ) )
	{
		track_processing_time ( *this );
		State ( Agent_e::HEALTHY );
		m_iPoolerTimeoutUS = sphMicroTimer () + 1000 * m_iMyQueryTimeoutMs;
		LazyDeleteOrChange ( m_iPoolerTimeoutUS ); // assign new time value, don't touch the handler
	}
}

/// prepare all necessary things to connect
/// assume socket is NOT connected
bool AgentConn_t::StartNextRetry ()
{
	sphLogDebugA ( "%d StartNextRetry() retries=%d, ref=%d", m_iStoreTag, m_iRetries, ( int ) GetRefcount () );
	m_iSock = -1;

	if ( m_pMultiAgent && !IsBlackhole () && m_iRetries>=0 )
	{
		m_tDesc.CloneFrom ( m_pMultiAgent->ChooseAgent () );
		SwitchBlackhole ();
	}

	if ( m_iRetries--<0 )
		return m_bManyTries ? Fail ( "retries limit exceeded" ) : false;

	sphLogDebugA ( "%d Connection %p, host %s, pers=%d", m_iStoreTag, this, m_tDesc.GetMyUrl().cstr(), m_tDesc.m_bPersistent );

	if ( IsPersistent() )
	{
		assert ( m_iSock==-1 );
		m_iSock = m_tDesc.m_pDash->m_pPersPool->RentConnection ();
		m_tDesc.m_bPersistent = m_iSock!=-2;
		if ( m_iSock>=0 && sphNBSockEof ( m_iSock ) )
			SafeCloseSocket ( m_iSock );
	}

	return true;
}

// if we're blackhole, drop retries, parser, reporter and return true
bool AgentConn_t::SwitchBlackhole ()
{
	if ( IsBlackhole () )
	{
		sphLogDebugA ( "%d Connection %p is blackhole (no retries, no parser, no reporter)", m_iStoreTag, this );
		if ( m_iRetries>0 )
			m_iRetries = 0;
		m_bManyTries = false;
		m_pParser = nullptr;
		m_pReporter = nullptr;
		return true;
	}
	return false;
}

// set up ll the stuff about async query. Namely - add timeout callback,
// initialize read/write task
void AgentConn_t::ScheduleCallbacks ()
{
	LazyTask ( m_iPoolerTimeoutUS, true, BYTE ( m_dIOVec.HasUnsent () ? 1 : 2 ) );
}

void FirePoller (); // forward definition

// retry timeout used when we need to pause before next retry, so just start connection when it fired
// hard timeout used when connection/query timed out. Drop existing connection and try again.
void AgentConn_t::TimeoutCallback ()
{
	SetNetLoop ();
	auto ePrevKind = m_eTimeoutKind;
	m_eTimeoutKind = TIMEOUT_UNKNOWN;

	// check if we accidentally orphaned (that is bug!)
	if ( CheckOrphaned() )
		return;

	switch ( ePrevKind )
	{
		case TIMEOUT_RETRY:
			if ( !DoQuery () )
				StartRemoteLoopTry ();
			FirePoller (); // fixme? M.b. no more necessary, since processing queue will restart on fired timeout.
			sphLogDebugA ( "%d finished retry timeout ref=%d", m_iStoreTag, ( int ) GetRefcount () );
			break;
		case TIMEOUT_HARD:
			if ( StateIs ( Agent_e::CONNECTING ) )
				Fatal ( eTimeoutsConnect, "connect timed out" );
			else
				Fatal ( eTimeoutsQuery, "query timed out" );
			StartRemoteLoopTry ();
			sphLogDebugA ( "%d <- hard timeout (ref=%d)", m_iStoreTag, ( int ) GetRefcount () );
			break;
		case TIMEOUT_UNKNOWN:
		default:
			sphLogDebugA ("%d Unknown kind of timeout invoked. No action", m_iStoreTag );
	}
}

// the reason for orphanes is suggested to be combined write, then read in netloop with epoll
bool AgentConn_t::CheckOrphaned()
{
	// check if we accidentally orphaned (that is bug!)
	if ( IsLast () && !IsBlackhole () )
	{
		sphLogDebug ( "Orphaned (last) connection detected!" );
		return true;
	}

	if ( m_pReporter && m_pReporter->IsDone () )
	{
		sphLogDebug ( "Orphaned (kind of done) connection detected!" );
		return true;
	}
	return false;
}

void AgentConn_t::AbortCallback()
{
	ReportFinish (false);
}

void AgentConn_t::ErrorCallback ( int64_t iWaited )
{
	SetNetLoop ();
	if ( !m_pPollerTask )
		return;
	m_iWaited += iWaited;

	int iErr = sphSockGetErrno ();
	Fatal ( eNetworkErrors, "detected the error (errno=%d, msg=%s)", iErr, sphSockError ( iErr ) );
	StartRemoteLoopTry ();
}

void AgentConn_t::SendCallback ( int64_t iWaited, DWORD uSent )
{
	SetNetLoop ();
	if ( !m_pPollerTask )
		return;

	if ( m_dIOVec.HasUnsent () )
	{
		m_iWaited += iWaited;
		if ( !SendQuery ( uSent ) )
			StartRemoteLoopTry ();
		sphLogDebugA ( "%d <- finished SendCallback (ref=%d)", m_iStoreTag, ( int ) GetRefcount () );
	}
}

void AgentConn_t::RecvCallback ( int64_t iWaited, DWORD uReceived )
{
	SetNetLoop ();
	if ( !m_pPollerTask )
		return;

	m_iWaited += iWaited;

	if ( !ReceiveAnswer ( uReceived ) )
		StartRemoteLoopTry ();
	sphLogDebugA ( "%d <- finished RecvCallback", m_iStoreTag );
}

/// if iovec is empty, prepare (build) the request.
void AgentConn_t::BuildData ()
{
	if ( m_pBuilder && m_dIOVec.IsEmpty () )
	{
		sphLogDebugA ( "%d BuildData for this=%p, m_pBuilder=%p", m_iStoreTag, this, m_pBuilder );
		// prepare our data to send.
		m_pBuilder->BuildRequest ( *this, m_tOutput );
		m_dIOVec.BuildFrom ( m_tOutput );
	} else
		sphLogDebugA ( "%d BuildData, already done", m_iStoreTag );
}

//! How many bytes we can read to m_pReplyCur (in bytes)
size_t AgentConn_t::ReplyBufPlace () const
{
	if ( !m_pReplyCur )
		return 0;
	if ( m_dReplyBuf.IsEmpty () )
		return m_dReplyHeader.begin () + REPLY_HEADER_SIZE - m_pReplyCur;
	return m_dReplyBuf.begin () + m_dReplyBuf.GetLength () - m_pReplyCur;
}

void AgentConn_t::InitReplyBuf ( int iSize )
{
	sphLogDebugA ( "%d InitReplyBuf ( %d )", m_iStoreTag, iSize );
	m_dReplyBuf.Reset ( iSize );
	if ( m_dReplyBuf.IsEmpty () )
	{
		m_iReplySize = -1;
		m_pReplyCur = m_dReplyHeader.begin ();
	} else {
		m_pReplyCur = m_dReplyBuf.begin ();
		m_iReplySize = iSize;
	}
}

#if USE_WINDOWS
void AgentConn_t::LeakRecvTo ( CSphFixedVector<BYTE>& dOut )
{
	assert ( dOut.IsEmpty () );
	if ( m_iReplySize<0 )
	{
		dOut.SwapData ( m_dReplyHeader );
		m_dReplyHeader.Reset ( REPLY_HEADER_SIZE );
	} else
		dOut.SwapData ( m_dReplyBuf );
	InitReplyBuf ();
}

void AgentConn_t::LeakSendTo ( CSphVector <ISphOutputBuffer* >& dOut, CSphVector<sphIovec>& dOutIO )
{
	assert ( dOut.IsEmpty () );
	assert ( dOutIO.IsEmpty () );
	m_tOutput.LeakTo ( dOut );
	m_dIOVec.LeakTo ( dOutIO );
}

#endif


/// raw (platform specific) send (scattered - from several buffers)
/// raw (platform specific) receive
	
#if USE_WINDOWS
inline SSIZE_T AgentConn_t::RecvChunk ()
{
	assert ( !m_pPollerTask->m_dRead.m_bInUse );
	if ( !m_pPollerTask )
		ScheduleCallbacks ();
	WSABUF dBuf;
	dBuf.buf = (CHAR*) m_pReplyCur;
	dBuf.len = (ULONG) ReplyBufPlace ();
	DWORD uFlags = 0;
	m_pPollerTask->m_dRead.Zero ();
	sphLogDebugA ( "%d Scheduling overlapped WSARecv for %d bytes", m_iStoreTag, ReplyBufPlace () );
	m_pPollerTask->m_dRead.m_bInUse = true;
	WSARecv ( m_iSock, &dBuf, 1, nullptr, &uFlags, &m_pPollerTask->m_dRead, nullptr );
	return -1;
}

inline SSIZE_T AgentConn_t::SendChunk ()
{
	assert ( !m_pPollerTask->m_dWrite.m_bInUse );
	SendingState ();
	if ( !m_pPollerTask )
		ScheduleCallbacks ();
	m_pPollerTask->m_dWrite.Zero ();
	sphLogDebugA ( "%d overlaped WSASend called for %d chunks", m_iStoreTag, m_dIOVec.IOSize () );
	m_pPollerTask->m_dWrite.m_bInUse = true;
	WSASend ( m_iSock, m_dIOVec.IOPtr (), (DWORD) m_dIOVec.IOSize (), nullptr, 0, &m_pPollerTask->m_dWrite, nullptr );
	return -1;
}

#else
inline SSIZE_T AgentConn_t::RecvChunk ()
{
	return sphSockRecv ( m_iSock, (char *) m_pReplyCur, ReplyBufPlace () );
}

inline SSIZE_T AgentConn_t::SendChunk ()
{
	struct msghdr dHdr = { 0 };
	dHdr.msg_iov = m_dIOVec.IOPtr ();
	dHdr.msg_iovlen = m_dIOVec.IOSize ();
	return ::sendmsg ( m_iSock, &dHdr, MSG_NOSIGNAL | MSG_DONTWAIT );
}
#endif

/// try to establish connection in the modern fast way, and also perform some data sending, if possible.
/// @return 1 on success, 0 if need fallback into usual (::connect), -1 on failure.
int AgentConn_t::DoTFO ( struct sockaddr * pSs, int iLen )
{
	if ( pSs->sa_family==AF_UNIX || g_iTFO==TFO_ABSENT || !(g_iTFO & TFO_CONNECT) )
		return 0;
	m_iStartQuery = sphMicroTimer (); // copied old behaviour
#if USE_WINDOWS
	if ( !ConnectEx )
		return 0;

	struct sockaddr_in sAddr;
	ZeroMemory ( &sAddr, sizeof ( sAddr ) );
	sAddr.sin_family = AF_INET;
	sAddr.sin_addr.s_addr = INADDR_ANY;
	sAddr.sin_port = 0;
	int iRes = bind ( m_iSock, (SOCKADDR*) &sAddr, sizeof ( sAddr ) );
	if ( iRes != 0 )
	{
		Fail ( "bind failed: %d %s", m_tDesc.m_sAddr.cstr () );
		return 0;
	}

#if defined ( TCP_FASTOPEN )
	int iOn = 1;
	iRes = setsockopt ( m_iSock, IPPROTO_TCP, TCP_FASTOPEN, (char*) &iOn, sizeof ( iOn ) );
	if ( iRes )
		sphLogDebug ( "setsockopt (TCP_FASTOPEN) failed: %s", sphSockError () );
	sphLogDebugA ( "%d TFO branch", m_iStoreTag );
	// fixme! ConnectEx doesn't accept scattered buffer. Need to prepare plain one for at least MSS size
#endif

	BuildData ();
	if ( !m_pPollerTask )
		ScheduleCallbacks ();
	sphLogDebugA ( "%d overlaped ConnectEx called", m_iStoreTag );
	m_pPollerTask->m_dWrite.Zero();
	// let us also send first chunk of the buff
	sphIovec * pChunk = m_dIOVec.IOPtr ();
	assert ( pChunk );
	assert ( !m_pPollerTask->m_dWrite.m_bInUse );
	m_pPollerTask->m_dWrite.m_bInUse = true;
	iRes = ConnectEx ( m_iSock, pSs, iLen, pChunk->buf, pChunk->len, NULL, &m_pPollerTask->m_dWrite );

	if ( iRes )
	{
		State ( Agent_e::CONNECTING );
		return 1;
	}

	int iErr = sphSockGetErrno ();
	if ( !IS_PENDING ( iErr ) )
	{
		Fatal ( eConnectFailures, "ConnectEx failed with %d, %s", iErr, sphSockError ( iErr ) );
		return -1;
	}
	State ( Agent_e::CONNECTING );
	return 1;
#else // USE_WINDOWS
#if defined (MSG_FASTOPEN)

	BuildData ();
	struct msghdr dHdr = { 0 };
	dHdr.msg_iov = m_dIOVec.IOPtr ();
	dHdr.msg_iovlen = m_dIOVec.IOSize ();
	dHdr.msg_name = pSs;
	dHdr.msg_namelen = iLen;

	auto iRes = ::sendmsg ( m_iSock, &dHdr, MSG_NOSIGNAL | MSG_FASTOPEN );
#elif defined (__APPLE__)
	struct sa_endpoints sAddr = { 0 };
	sAddr.sae_dstaddr = pSs;
	sAddr.sae_dstaddrlen = iLen;

	BuildData ();
	size_t iSent = 0;
	auto iRes = connectx ( m_iSock, &sAddr, SAE_ASSOCID_ANY, CONNECT_RESUME_ON_READ_WRITE | CONNECT_DATA_IDEMPOTENT
						   , m_dIOVec.IOPtr (), m_dIOVec.IOSize (), &iSent, nullptr );

	if ( iSent )
	{
		sphLogDebugv ( "Mac OS TFO: advancing to %zu with %d, error %d", iSent, iRes, sphSockGetErrno () );
		m_dIOVec.StepForward ( iSent );
		if ( iRes<0 && m_dIOVec.HasUnsent () )
			iRes = 0;
	}
#else
	int iRes = 0;
	return iRes;
#endif
	if ( iRes>=0 ) // lucky; we already sent something!
	{
		track_processing_time ( *this );
		sphLogDebugA ( "%d sendmsg/connectx returned %d", m_iStoreTag, ( size_t ) iRes );
		sphLogDebugv ( "TFO send succeeded, %zu bytes sent", ( size_t ) iRes );
		// now 'connect' and 'query' merged, so timeout became common.
		m_iPoolerTimeoutUS += 1000*m_iMyQueryTimeoutMs;
		return SendQuery ( iRes ) ? 1 : -1;
	}

	auto iErr = sphSockGetErrno ();
	if ( iErr!=EINPROGRESS )
	{
		if ( iErr==EOPNOTSUPP )
		{
			assert ( g_iTFO!=TFO_ABSENT );
			sphWarning("TFO client supoport unavailable, switch to usual connect()");
			g_iTFO &= ~TFO_CONNECT;
			return 0;
		}
		Fatal ( eConnectFailures, "sendmsg/connectx() failed: errno=%d, %s", iErr, sphSockError ( iErr ) );
		return -1;
	}
	sphLogDebugA ( "%d TFO returned EINPROGRESS (usuall connect in game; scheduling callbacks)", m_iStoreTag );
	ScheduleCallbacks ();
	return 1;
#endif
}

//! Simplified wrapper for ScheduleDistrJobs, wait for finish and return succeeded
int PerformRemoteTasks ( VectorAgentConn_t &dRemotes, RequestBuilder_i * pQuery, ReplyParser_i * pParser )
{
	if ( dRemotes.IsEmpty() )
		return 0;

	CSphRefcountedPtr<RemoteAgentsObserver_i> tReporter { GetObserver () };
	ScheduleDistrJobs ( dRemotes, pQuery, pParser, tReporter );
	tReporter->Finish ();
	return (int)tReporter->GetSucceeded ();
}


/// Add set of works (dRemotes) to the queue.
/// jobs themselves are ref-counted and owned by nobody (they're just released on finish, so
/// if nobody waits them (say, blackhole), they just dissapeared).
/// on return blackholes removed from dRemotes
void ScheduleDistrJobs ( VectorAgentConn_t &dRemotes, RequestBuilder_i * pQuery, ReplyParser_i * pParser,
	Reporter_i * pReporter, int iQueryRetry, int iQueryDelay )
{
//	sphLogSupress ( "L ", SPH_LOG_VERBOSE_DEBUG );
//	sphLogSupress ( "- ", SPH_LOG_VERBOSE_DEBUG );
//	TimePrefixed::TimeStart();
	assert ( pReporter );
	sphLogDebugv ( "S ==========> ScheduleDistrJobs() for %d remotes", dRemotes.GetLength () );

	if ( dRemotes.IsEmpty () )
	{
		sphWarning ("Empty remotes provided to ScheduleDistrJobs. Consider to save resources and avoid it");
		return;
	}

	bool bNeedKick = false; // if some of connections falled to waiting and need to kick the poller.
	ARRAY_FOREACH ( i, dRemotes )
	{
		auto & pConnection = dRemotes[i];
		pConnection->GenericInit ( pQuery, pParser, pReporter, iQueryRetry, iQueryDelay );

		// start the actual job.
		// It might lucky be completed immediately. Or, it will be acquired by async network
		// (and addreffed there in the loop)
		pReporter->FeedTask ( true );
		pConnection->StartRemoteLoopTry ();
		bNeedKick |= pConnection->FireKick ();

		// remove and release blackholes from the queue.
		if ( pConnection->IsBlackhole () )
		{
			sphLogDebugv ( "S Remove blackhole()" );
			SafeRelease ( pConnection );
			dRemotes.RemoveFast ( i-- );
			pReporter->FeedTask ( false );
		}
	}

	if ( bNeedKick )
	{
		sphLogDebugA ( "A Events need fire. Do it..." );
		FirePoller ();
	}

	sphLogDebugv ( "S ScheduleDistrJobs() done. Total %d", dRemotes.GetLength () );
}

class ReportCallback_c : public Reporter_i
{
public:
	template <typename CALLBACK_F>
	ReportCallback_c ( CALLBACK_F&& fAction, AgentConn_t* pConnection )
		: m_pCallback { std::forward<CALLBACK_F> ( fAction ) }
		, m_pConnection ( pConnection )
	{}

	void FeedTask ( bool bAdd ) final
	{
		if ( bAdd )
			SafeAddRef ( m_pConnection )
		else
			SafeRelease ( m_pConnection ) // yes, SafeRelease will make m_pConnection=0 at the end!
	};

	void Report ( bool bParam ) final
	{
		assert ( m_pConnection && "strange Report called without the connection!" );
		SafeRelease ( m_pConnection )
		if ( m_pCallback )
			m_pCallback ( bParam );
	};

	// just always return 'false' here to suppress any kind of 'orphaned' stuff.
	bool IsDone () const final { return false; };

private:
	using BoolCallBack_f = std::function<void ( bool )>;

	BoolCallBack_f	m_pCallback = nullptr;
	AgentConn_t	*	m_pConnection = nullptr;
};



/// Schedule one job (remote). Callback pAction will be called with bool 'success' param on finish.
/// If connection is blackhole, returns false, and callback will NOT be called in the case
/// (since blackholes live alone).
bool RunRemoteTaskImpl ( AgentConn_t* pConnection, RequestBuilder_i* pQuery, ReplyParser_i* pParser,
	Reporter_i* pReporter, int iQueryRetry, int iQueryDelay )
{
	sphLogDebugv ( "S ==========> ScheduleDistrJob()" );
	pConnection->GenericInit ( pQuery, pParser, pReporter, iQueryRetry, iQueryDelay );

	// start the actual job.
	// It might lucky be completed immediately. Or, it will be acquired by async network
	// (and addreffed there in the loop)
	pReporter->FeedTask ( true );
	pConnection->StartRemoteLoopTry ();
	bool bNeedKick = pConnection->FireKick (); // if connection falled to waiting and need to kick the poller.

	// blackhole is not traced; remove it
	bool bBlackhole = pConnection->IsBlackhole ();
	if ( bBlackhole)
	{
		sphLogDebugv ( "S Remove blackhole()" );
		pReporter->FeedTask ( false );
	}

	if ( bNeedKick )
	{
		sphLogDebugA ( "A Events need fire. Do it..." );
		FirePoller ();
	}

	sphLogDebugv ( "S ScheduleDistrJob() done. Result is %d", bBlackhole?0:1);
	return !bBlackhole;
}

bool RunRemoteTask ( AgentConn_t* pConnection, RequestBuilder_i* pQuery, ReplyParser_i* pParser,
	Deffered_f&& pAction, int iQueryRetry, int iQueryDelay )
{
	CSphRefcountedPtr<Reporter_i> pReporter (
		new ReportCallback_c ( std::move ( pAction ), pConnection ));
	return RunRemoteTaskImpl ( pConnection, pQuery, pParser, pReporter, iQueryRetry, iQueryDelay );
}

bool RunRemoteTask ( AgentConn_t* pConnection, RequestBuilder_i* pQuery, ReplyParser_i* pParser,
	const Deffered_f& pAction, int iQueryRetry, int iQueryDelay )
{
	CSphRefcountedPtr<Reporter_i> pReporter (
		new ReportCallback_c ( pAction, pConnection ));
	return RunRemoteTaskImpl ( pConnection, pQuery, pParser, pReporter, iQueryRetry, iQueryDelay );
}

// this is run once entering query loop for all retries (and all mirrors).
void AgentConn_t::GenericInit ( RequestBuilder_i * pQuery, ReplyParser_i * pParser,
								Reporter_i * pReporter, int iQueryRetry, int iQueryDelay )
{
	sphLogDebugA ( "%d GenericInit() pBuilder %p, parser %p, retries %d, delay %d, ref=%d",
		m_iStoreTag, pQuery, pParser, iQueryRetry, iQueryDelay, ( int ) GetRefcount ());
	if ( iQueryDelay>=0 )
		m_iDelay = iQueryDelay;

	m_pBuilder = pQuery;
	m_iWall = 0;
	m_iWaited = 0;
	m_bNeedKick = false;
	m_pPollerTask = nullptr;
	if ( m_pMultiAgent || !SwitchBlackhole() )
	{
		SafeAddRef ( pReporter );
		m_pReporter = pReporter;
		m_pParser = pParser;
		if ( iQueryRetry>=0 )
			m_iRetries = iQueryRetry * m_iMirrorsCount;
		m_bManyTries = m_iRetries>0;
	}

	SetNetLoop ( false );
	State ( Agent_e::HEALTHY );
}

/// an entry point to the whole remote agent's work
void AgentConn_t::StartRemoteLoopTry ()
{
	sphLogDebugA ( "%d StartRemoteLoopTry() ref=%d", m_iStoreTag, ( int ) GetRefcount () );
	while ( StartNextRetry () )
	{
		/// reset state before every retry
		m_dIOVec.Reset ();
		m_tOutput.Reset ();
		InitReplyBuf ();
		m_bConnectHandshake = true;
		m_bSuccess = 0;
		m_iStartQuery = 0;
		m_pPollerTask = nullptr;

		if ( StateIs ( Agent_e::RETRY ) )
		{
			assert ( !IsBlackhole () ); // blackholes never uses retry!
			assert ( !m_pPollerTask ); // must be cleaned out before try!

			// here we can came not only after delay, but also immediately (if iDelay==0)
			State ( Agent_e::HEALTHY );

			if ( m_iDelay>0 )
			{
				// can't start right now; need to postpone until timeout
				sphLogDebugA ( "%d postpone DoQuery() for %d msecs", m_iStoreTag, m_iDelay );
				LazyTask ( sphMicroTimer () + 1000 * m_iDelay, false );
				return;
			}
		}

		if ( DoQuery () )
			return;
	};
	ReportFinish ( false );
	sphLogDebugA ( "%d StartRemoteLoopTry() finished ref=%d", m_iStoreTag, ( int ) GetRefcount () );
}

// do oneshot query. Return true on any success
bool AgentConn_t::DoQuery()
{
	sphLogDebugA ( "%d DoQuery() ref=%d", m_iStoreTag, ( int ) GetRefcount () );
	auto iNow = sphMicroTimer ();
	if ( m_iSock>=0 )
	{
		sphLogDebugA ( "%d branch for established(%d). Timeout %d", m_iStoreTag, m_iSock, m_iMyQueryTimeout );
		m_bConnectHandshake = false;
		m_pReplyCur += sizeof ( int );
		m_iStartQuery = iNow; /// copied from old behaviour
		m_iPoolerTimeoutUS = iNow + 1000 * m_iMyQueryTimeoutMs;
		return SendQuery ();
	}

	// fill initial chunks
	m_tOutput.SendDword ( SPHINX_CLIENT_VERSION );
	m_tOutput.StartNewChunk ();
	if ( IsPersistent() && m_iSock==-1 )
	{
		{
			auto tHdr = APIHeader ( m_tOutput, SEARCHD_COMMAND_PERSIST );
			m_tOutput.SendInt ( 1 ); // set persistent to 1.
		}
		m_tOutput.StartNewChunk ();
	}

	sphLogDebugA ( "%d branch for not established. Timeout %d", m_iStoreTag, m_iMyConnectTimeout );
	m_iPoolerTimeoutUS = iNow + 1000 * m_iMyConnectTimeoutMs;
	if ( !m_tDesc.m_bNeedResolve )
		return EstablishConnection ();

	// here we're in case agent's remote addr need to be resolved (DNS required)
	assert ( m_tDesc.m_iFamily==AF_INET );
	AddRef ();
	sphLogDebugA ( "%d -> async GetAddress_a scheduled() ref=%d", m_iStoreTag, ( int ) GetRefcount () );
	DNSResolver_c::GetAddress_a ( m_tDesc.m_sAddr.cstr (), [this] ( DWORD uIP )
	{
		sphLogDebugA ( "%d :- async GetAddress_a callback (ip is %u) ref=%d", m_iStoreTag, uIP, ( int ) GetRefcount () );
		m_tDesc.m_uAddr = uIP;
		if ( !EstablishConnection () )
			StartRemoteLoopTry ();
		sphLogDebugA ( "%d <- async GetAddress_a returned() ref=%d", m_iStoreTag, ( int ) GetRefcount () );
		if ( FireKick () )
			FirePoller ();
		Release ();
	} );

	// for blackholes we parse query immediately, since builder will be disposed
	// outside once we returned from the function
	if ( IsBlackhole () )
		BuildData ();
	return true;
}

// here ip resolved; socket is NOT connected.
// We can initiate connect, or even send the chunk using TFO.
bool AgentConn_t::EstablishConnection ()
{
	sphLogDebugA ( "%d EstablishConnection() ref=%d", m_iStoreTag, ( int ) GetRefcount () );
	// first check if we're in bounds of timeout.
	// usually it is done by outside callback, however in case of deffered DNS we may be here out of sync and so need
	// to check it explicitly.
	if ( m_iPoolerTimeoutUS<sphMicroTimer () )
		return Fatal ( eConnectFailures, "connect timeout reached resolving address for %s", m_tDesc.m_sAddr.cstr () );

	if ( m_tDesc.m_iFamily==AF_INET && !m_tDesc.m_uAddr )
		return Fatal ( eConnectFailures, "can't get address for %s", m_tDesc.m_sAddr.cstr () );

	assert (m_iSock==-1); ///< otherwize why we're here?

	socklen_t len = 0;
	sockaddr_storage ss = {0};
	ss.ss_family = m_tDesc.m_iFamily;

	if ( ss.ss_family==AF_INET )
	{
		auto * pIn = ( struct sockaddr_in * ) &ss;
		pIn->sin_port = htons ( ( unsigned short ) m_tDesc.m_iPort );
		pIn->sin_addr.s_addr = m_tDesc.m_uAddr;
		len = sizeof ( *pIn );
	}
#if !USE_WINDOWS
	else if ( ss.ss_family==AF_UNIX )
	{
		auto * pUn = ( struct sockaddr_un * ) &ss;
		strncpy ( pUn->sun_path, m_tDesc.m_sAddr.cstr (), sizeof ( pUn->sun_path ) );
		len = sizeof ( *pUn );
	}
#endif

	m_iSock = socket ( m_tDesc.m_iFamily, SOCK_STREAM, 0 );
	sphLogDebugA ( "%d Created new socket %d", m_iStoreTag, m_iSock );

	if ( m_iSock<0 )
		return Fatal ( eConnectFailures, "socket() failed: %s", sphSockError () );

	if ( sphSetSockNB ( m_iSock )<0 )
		return Fatal ( eConnectFailures, "sphSetSockNB() failed: %s", sphSockError () );

	// connection in progress
	State ( Agent_e::CONNECTING );

	// prepare our data to send.
	auto iTfoRes = DoTFO ( ( struct sockaddr * ) &ss, len );
	if ( iTfoRes==1 )
		return true;
	else if ( iTfoRes==-1 )
		return false;

	m_iStartQuery = sphMicroTimer (); // copied old behaviour
	sphLogDebugA ( "%d usual ::connect invoked for %d", m_iStoreTag, m_iSock );
	int iRes = ::connect ( m_iSock, ( struct sockaddr * ) &ss, len );
	if ( iRes<0 )
	{
		int iErr = sphSockGetErrno ();
		if ( iErr==EINTR || !IS_PENDING_PROGRESS ( iErr ) ) // check for EWOULDBLOCK is for winsock only
			return Fatal ( eConnectFailures, "connect() failed: errno=%d, %s", iErr, sphSockError ( iErr ) );
	}
	return SendQuery ();
}

/// send query (whole, or chunk)
/// if data is sent by external routine, param says how many bytes are sent.
bool AgentConn_t::SendQuery ( DWORD uSent )
{
	sphLogDebugA ( "%d SendQuery() ref=%d", m_iStoreTag, ( int ) GetRefcount () );

	// here we have connected socket and are in process of sending blob there.
	// prepare our data to send.
	if ( !uSent )
		BuildData ();
	SSIZE_T iRes = 0;
	while ( m_dIOVec.HasUnsent () )
	{
		if ( !uSent )
			iRes = SendChunk ();
		else
		{
			iRes = uSent;
			uSent = 0;
		}

		if ( iRes==-1 )
			break;
		sphLogDebugA ( "%d sent %d bytes", m_iStoreTag, (int)iRes );
		m_dIOVec.StepForward ( iRes );
		if ( iRes>0 )
			SendingState ();
	}

	if ( !m_dIOVec.HasUnsent () ) // we've abandoned output queue
	{
		sphLogDebugA ( "%d sending finished", m_iStoreTag );
		DisableWrite();
		return ReceiveAnswer ();
	}

	assert ( iRes==-1 );

	int iErr = sphSockGetErrno ();
	if ( !IS_PENDING_PROGRESS(iErr) )
	{
		if ( !( iErr==ENOTCONN && StateIs ( Agent_e::CONNECTING ) ) )
			return Fatal ( eNetworkErrors, "error when sending data: %s", sphSockError ( iErr ) );
		else
			sphLogDebugA ( "%d Not connected, schedule... ref=%d", m_iStoreTag, ( int ) GetRefcount () );
	}

	sphLogDebugA ( "%d -> Schedule sender ref=%d", m_iStoreTag, ( int ) GetRefcount () );
	ScheduleCallbacks ();
	return true;
}

// here we fall when we're expect to read something from remote side
bool AgentConn_t::ReceiveAnswer ( DWORD uRecv )
{
	sphLogDebugA ( "%d ReceiveAnswer() ref=%d", m_iStoreTag, ( int ) GetRefcount () );
	// check if we just read anything, of already read something and have to continue.
	SSIZE_T iRes = 0;
	while ( ReplyBufPlace () )
	{
		if ( uRecv )
		{
			iRes = uRecv;
			uRecv = 0;
		} else
			iRes = RecvChunk();

		if ( iRes<=0 )
			break;

		m_pReplyCur += iRes;
		auto iRest = ReplyBufPlace ();
		sphLogDebugA ( "%d RecvChunk returned %d (%d bytes rest in input buffer)", m_iStoreTag, ( int ) iRes, iRest );

		// We're in state of receiving the header (m_iReplySize==-1 is the indicator)
		if ( IsReplyHeader () && iRest<=( REPLY_HEADER_SIZE - 4 ))
		{
			MemInputBuffer_c dBuf ( m_dReplyHeader.begin(), REPLY_HEADER_SIZE );
			auto iVer = dBuf.GetInt ();
			sphLogDebugA ( "%d Handshake is %d (this message may appear >1 times)", m_iStoreTag, iVer );

			// parse handshake answer (if necessary)
			if ( m_bConnectHandshake && iVer!=SPHINX_SEARCHD_PROTO && iVer!=0x01000000UL )
				return Fatal ( eWrongReplies, "handshake failure (unexpected protocol version=%d)", iVer );

			if ( !iRest ) // not only handshake, but whole header is here
			{
				auto uStat = dBuf.GetWord ();
				auto VARIABLE_IS_NOT_USED uVer = dBuf.GetWord (); // there is version here. But it is not used.
				auto iReplySize = dBuf.GetInt ();

				sphLogDebugA ( "%d Header (Status=%d, Version=%d, answer need %d bytes)", m_iStoreTag, uStat, uVer, iReplySize );

				if ( iReplySize<0
					|| iReplySize>g_iMaxPacketSize ) // FIXME! add reasonable max packet len too
					return Fatal ( eWrongReplies, "invalid packet size (status=%d, len=%d, max_packet_size=%d)"
								   , uStat, iReplySize, g_iMaxPacketSize );

				// allocate buf for reply
				InitReplyBuf ( iReplySize );
				m_eReplyStatus = ( SearchdStatus_e ) uStat;
			}
		}
	}

	if ( !ReplyBufPlace () ) // we've received full reply
	{
		auto bRes = CommitResult ();
		if ( bRes )
			ReportFinish ( true );
		return bRes;
	}

	if ( !iRes ) // timeout or eof happens; retry.
		return Fatal ( eUnexpectedClose, "agent closed connection" );

	assert ( iRes==-1 );

	int iErr = sphSockGetErrno ();
	if ( !IS_PENDING ( iErr ) )
		return Fatal ( eNetworkErrors, "receiving failure (errno=%d, msg=%s)", iErr, sphSockError ( iErr ) );

	ScheduleCallbacks();
	return true;
}

// when full blob with expected size is received...
// just a fine: parse the answer, collect results, dispose agent as one is done.
bool AgentConn_t::CommitResult ()
{
	sphLogDebugA ( "%d CommitResult() ref=%d, parser %p", m_iStoreTag, ( int ) GetRefcount (), m_pParser );
	if ( !m_pParser )
	{
		Finish();
		return true;
	}

	if ( m_pReporter && m_pReporter->IsDone ())
	{
		Finish ();
		sphLogDebug ( "Orphaned (kind of done) connection detected!" );
		return true;
	}

	MemInputBuffer_c tReq ( m_dReplyBuf.Begin (), m_iReplySize );

	if ( m_eReplyStatus == SEARCHD_RETRY )
	{
		m_sFailure.SetSprintf ( "remote warning: %s", tReq.GetString ().cstr () );
		return BadResult ( -1 );
	}

	if ( m_eReplyStatus == SEARCHD_ERROR )
	{
		m_sFailure.SetSprintf ( "remote error: %s", tReq.GetString ().cstr () );
		return BadResult ( -1 );
	}

	bool bWarnings = ( m_eReplyStatus == SEARCHD_WARNING );
	if ( bWarnings )
		m_sFailure.SetSprintf ( "remote warning: %s", tReq.GetString ().cstr () );

	if ( !m_pParser->ParseReply ( tReq, *this ) )
		return BadResult ();

	Finish();

	if ( !bWarnings && m_pResult )
		bWarnings = m_pResult->HasWarnings ();

	agent_stats_inc ( *this, bWarnings ? eNetworkCritical : eNetworkNonCritical );
	m_bSuccess = 1;
	return true;
}


void AgentConn_t::SetMultiAgent ( MultiAgentDesc_c * pAgent )
{
	assert ( pAgent );
	pAgent->AddRef ();
	m_pMultiAgent = pAgent;
	m_iMirrorsCount = pAgent->GetLength ();
	m_iRetries = pAgent->GetRetryLimit ();
	m_bManyTries = m_iRetries>0;
}

#if 0

// here is async dns resolution made on mac os

//#include <CoreFoundation/CoreFoundation.h>
#include <CFNetwork/CFNetwork.h>

// just a template based on https://developer.apple.com/library/content/documentation/NetworkingInternet/Conceptual/NetworkingTopics/Articles/ResolvingDNSHostnames.html
void clbck ( CFHostRef theHost, CFHostInfoType typeInfo
			 , const CFStreamError * error
			 , void * info)
{
	Boolean bOk;
	auto arr = CFHostGetAddressing( theHost, &bOk );
	if ( bOk )
	{
		auto iSize = CFArrayGetCount ( arr );
		for ( auto i=0; i<iSize; ++i )
		{
			auto * pValue = ( CFDataRef )CFArrayGetValueAtIndex (arr, i);
			struct sockaddr_in * remoteAddr = ( struct sockaddr_in * ) CFDataGetBytePtr ( pValue );

		}

	}
}

// see also https://stackoverflow.com/questions/1363787/is-it-safe-to-call-cfrunloopstop-from-another-thread

void macresolver()
{
	CFStringRef a = CFStringCreateWithCString ( kCFAllocatorDefault, "hello.world.com", kCFStringEncodingUTF8 );
	auto hostRef = CFHostCreateWithName ( kCFAllocatorDefault, a );
	CFRelease (a);

	CFHostClientContext ctx { 0 };
	// ctx.info = this
	Boolean set_ok = CFHostSetClient ( hostRef, &clbck, &ctx );

	CFHostScheduleWithRunLoop ( hostRef, CFRunLoopGetCurrent (), kCFRunLoopCommonModes );

	CFStreamError sError;
	Boolean is_resolving = CFHostStartInfoResolution ( hostRef, kCFHostAddresses, &sError );
	// CFRelease ( hostRef ); // fixme! where to put it? Here? Or in callback?
}

#endif


PollableEvent_t::PollableEvent_t ()
{
	int iRead = -1;
	int iWrite = -1;
#if HAVE_EVENTFD
	int iFD = eventfd ( 0, EFD_NONBLOCK );
	if ( iFD==-1 )
		m_sError.SetSprintf ( "failed to create eventfd: %s", strerrorm ( errno ) );
	iRead = iWrite = iFD;
#else
	CreateSocketPair ( iRead, iWrite, m_sError );
#endif

	if ( iRead==-1 || iWrite==-1 )
		sphWarning ( "PollableEvent_t create error:%s", m_sError.cstr () );
	m_iPollablefd = iRead;
	m_iSignalEvent = iWrite;
}

PollableEvent_t::~PollableEvent_t ()
{
	Close ();
}

void PollableEvent_t::Close ()
{
	SafeCloseSocket ( m_iPollablefd );
#if !HAVE_EVENTFD
	SafeCloseSocket ( m_iSignalEvent );
#endif
}

int PollableEvent_t::PollableErrno ()
{
	return sphSockGetErrno ();
}

bool PollableEvent_t::FireEvent () const
{
	if ( m_iSignalEvent==-1 )
		return true;

	int iErrno = EAGAIN;
	while ( iErrno==EAGAIN || iErrno==EWOULDBLOCK )
	{
		uint64_t uVal = 1;
#if HAVE_EVENTFD
		int iPut = ::write ( m_iSignalEvent, &uVal, 8 );
#else
		int iPut = sphSockSend ( m_iSignalEvent, ( const char * ) &uVal, 8 );
#endif
		if ( iPut==8 )
			return true;
		iErrno = PollableErrno ();
	};
	return false;
}

// just wipe-out a fired event to free queue, we don't need the value itself
void PollableEvent_t::DisposeEvent () const
{
	assert ( m_iPollablefd!=-1 );
	uint64_t uVal = 0;
	while ( true )
	{
#if HAVE_EVENTFD
		auto iRead = ::read ( m_iPollablefd, &uVal, 8 );
		if ( iRead==8 )
			break;
#else
		// socket-pair case might stack up some values and these should be read
		int iRead = sphSockRecv ( m_iPollablefd, ( char * ) &uVal, 8 );
		if ( iRead<=0 )
			break;
#endif
	}
}


struct Task_t:
#if USE_WINDOWS
	DoubleOverlapped_t,
#endif
	EnqueuedTimeout_t
{
	enum IO : BYTE { NO = 0, RW = 1, RO = 2 };

	AgentConn_t *	m_pPayload	= nullptr;	// ext conn we hold
	int64_t			m_iPlannedTimeout = 0;	// asked timeout (-1 - delete task, 0 - no changes; >0 - set value)
	int				m_ifd = -1;
	int 			m_iStoredfd = -1;		// helper to find original fd if socket was closed
	int				m_iTickProcessed=0;		// tick # to detect and avoid reading callback in same loop with write
	BYTE			m_uIOActive = NO;		// active IO callbacks: 0-none, 1-r+w, 2-r
	BYTE			m_uIOChanged = NO;		// need IO changes: dequeue (if !m_uIOActive), 1-set to rw, 2-set to ro
};

ThreadRole LazyThread;

// low-level ops depends from epoll/kqueue/iocp availability

#if USE_WINDOWS
class NetEvent_c
{
	Task_t *			m_pTask = nullptr;
	bool				m_bWrite = false;
	DWORD				m_uNumberOfBytesTransferred = 0;
	bool				m_bSignaler = true;

public:
	NetEvent_c ( LPOVERLAPPED_ENTRY pEntry )
	{
		if ( !pEntry )
			return;
		
		m_bSignaler = !pEntry->lpOverlapped;
		if ( m_bSignaler )
			return;

		auto pOvl = (SingleOverlapped_t *) pEntry->lpOverlapped;
		auto uOffset = pOvl->m_uParentOffset;
		m_pTask = (Task_t *) ( (BYTE*) pOvl - uOffset );
		m_bWrite = uOffset<sizeof ( OVERLAPPED );
		m_uNumberOfBytesTransferred = pEntry->dwNumberOfBytesTransferred;

		if ( m_pTask )
		{
			assert ( pOvl->m_bInUse );
			pOvl->m_bInUse = false;
			if ( m_pTask->m_ifd==-1 && m_pTask->m_pPayload==nullptr && !m_pTask->IsInUse() )
			{
				sphLogDebugL ( "L Removing deffered %p", m_pTask );
				SafeDelete ( m_pTask );
			}
		}
	}

	inline Task_t * GetTask () const
	{
		return m_pTask;
	}

	inline bool IsSignaler () const
	{
		return m_bSignaler;
	}

	inline int GetEvents() const
	{
		// return 0 for internal signal, or 1 for write, 1+sizeof(OVERLAPPED) for read.
		return (!!m_pTask) + 2 * (!!m_bWrite);
	}

	inline bool IsError() const
	{
		return false;
	}

	inline bool IsEof() const
	{
		return !m_bWrite && !m_uNumberOfBytesTransferred;
	}

	inline bool IsRead() const
	{
		return !m_bWrite;
	}

	inline bool IsWrite() const
	{
		return m_bWrite;
	}

	inline DWORD BytesTransferred () const
	{
		return m_uNumberOfBytesTransferred;
	}
};

#elif POLLING_EPOLL

class NetEvent_c
{
	struct epoll_event * m_pEntry = nullptr;
	const Task_t * m_pSignalerTask = nullptr;

public:
	NetEvent_c ( struct epoll_event * pEntry, const Task_t * pSignaler )
		: m_pEntry ( pEntry )
		, m_pSignalerTask ( pSignaler )
	{}

	inline Task_t * GetTask ()
	{
		assert ( m_pEntry );
		return ( Task_t * ) m_pEntry->data.ptr;
	}

	inline bool IsSignaler ()
	{
		assert ( m_pEntry );
		auto pTask = GetTask ();
		if ( pTask==m_pSignalerTask )
		{
			auto pSignaler = ( PollableEvent_t * ) m_pSignalerTask->m_pPayload;
			pSignaler->DisposeEvent ();
		}
		return pTask==m_pSignalerTask;
	}

	inline int GetEvents ()
	{
		assert ( m_pEntry );
		return m_pEntry->events;
	}

	inline bool IsError ()
	{
		assert ( m_pEntry );
		return ( m_pEntry->events & EPOLLERR )!=0;
	}

	inline bool IsEof ()
	{
		assert ( m_pEntry );
		return ( m_pEntry->events & EPOLLHUP )!=0;
	}

	inline bool IsRead ()
	{
		assert ( m_pEntry );
		return ( m_pEntry->events & EPOLLIN )!=0;
	}

	inline bool IsWrite ()
	{
		assert ( m_pEntry );
		return ( m_pEntry->events & EPOLLOUT )!=0;
	}

	inline DWORD BytesTransferred ()
	{
		assert ( m_pEntry );
		return 0;
	}
};

#elif POLLING_KQUEUE

class NetEvent_c
{
	struct kevent * m_pEntry = nullptr;
	const Task_t * m_pSignalerTask = nullptr;

public:
	NetEvent_c ( struct kevent * pEntry, const Task_t * pSignaler )
		: m_pEntry ( pEntry )
		, m_pSignalerTask ( pSignaler )
	{}

	inline Task_t * GetTask ()
	{
		assert ( m_pEntry );
		return ( Task_t * ) m_pEntry->udata;
	}

	inline bool IsSignaler ()
	{
		assert ( m_pEntry );
		auto pTask = GetTask();
		if ( pTask==m_pSignalerTask )
		{
			auto pSignaler = ( PollableEvent_t* )m_pSignalerTask->m_pPayload;
			pSignaler->DisposeEvent ();
		}
		return pTask==m_pSignalerTask;
	}

	inline int GetEvents ()
	{
		assert ( m_pEntry );
		return m_pEntry->filter;
	}

	inline bool IsError ()
	{
		assert ( m_pEntry );
		if ( ( m_pEntry->flags & EV_ERROR )==0 )
			return false;

		sphLogDebugL ( "L error for %lu, errno=%lu, %s", m_pEntry->ident, m_pEntry->data, sphSockError (
			m_pEntry->data ) );
		return true;
	}

	inline bool IsEof ()
	{
		assert ( m_pEntry );
		return ( m_pEntry->flags & EV_EOF )!=0;
	}

	inline bool IsRead ()
	{
		assert ( m_pEntry );
		return ( m_pEntry->filter==EVFILT_READ )!=0;
	}

	inline bool IsWrite ()
	{
		assert ( m_pEntry );
		return ( m_pEntry->filter==EVFILT_WRITE )!=0;
	}

	inline DWORD BytesTransferred ()
	{
		assert ( m_pEntry );
		return 0;
	}
};
#endif

class NetEventsFlavour_c
{
protected:
	int m_iEvents = 0;    ///< how many events are in queue.
	static const int m_iCReserve = 256; 	/// will always provide extra that space of events to poller

	// perform actual changing OR scheduling of pTask subscription state (on BSD we collect changes and will populate later)
	// NOTE! m_uIOChanged==0 field here means active 'unsubscribe' (in use for deletion)
void events_change_io ( Task_t * pTask )
	{
		assert ( pTask );

		// check if 'pure timer' deletion asked (i.e. task which didn't have io at all)
		if ( !pTask->m_uIOActive && !pTask->m_uIOChanged )
		{
			sphLogDebugL ( "L events_change_io invoked for pure timer (%p); nothing to do (m_ifd, btw, is %d)", pTask, pTask->m_ifd );
			return;
		}

		auto iEvents = events_apply_task_changes ( pTask );
		m_iEvents += iEvents;
		pTask->m_uIOActive = pTask->m_uIOChanged;
		pTask->m_uIOChanged = Task_t::NO;
		sphLogDebugL ( "L events_apply_task_changes returned %d, now %d events counted", iEvents, m_iEvents );
	}
protected:
#if USE_WINDOWS
						  // for windows it is one more level of indirection:
						  // we set and wait for the tasks in one thread,
						  // and iocp also have several working threads.
	HANDLE				m_IOCP = INVALID_HANDLE_VALUE;
	CSphVector<DWORD>	m_dIOThreads;
	CSphVector<OVERLAPPED_ENTRY>	m_dReady;

	inline void events_create ( int iSizeHint )
	{
		// fixme! m.b. more workers, or just one enough?
		m_IOCP = CreateIoCompletionPort ( INVALID_HANDLE_VALUE, NULL, 0, 1 );
		sphLogDebugL ( "L IOCP %d created", m_IOCP );
		m_dReady.Reserve (m_iCReserve + iSizeHint);
	}

	inline void events_destroy ()
	{
		sphLogDebugv ( "iocp poller %d closed", m_IOCP );
		// that is have to be done only when ALL reference sockets are closed.
		// what about persistent? Actually we have to first close them, then close iocp.
		// m.b. on finish of the daemon that is not so important, but just mentioned to be known.
		CloseHandle ( m_IOCP );
	}

	inline void fire_event ()
	{
		if ( !PostQueuedCompletionStatus ( m_IOCP, 0, 0, 0 ) )
			sphLogDebugv ( "L PostQueuedCompletionStatus failed with error %d", GetLastError () );
	}

private:
	// each action added one-by-one...
	int events_apply_task_changes ( Task_t * pTask )
	{
		// if socket already closed (say, FATAL in action),
		// we don't need to unsubscribe events, but still need to return num of deleted events
		// to keep in health poller's input buffer
		
		bool bApply = pTask->m_ifd!=-1;
		
		if ( !pTask->m_uIOChanged ) // requested delete
		{
			sphLogDebugL ( "L request to remove event (%d), %d events rest", pTask->m_ifd, m_iEvents );
			if ( pTask->IsInUse () && pTask->m_pPayload && bApply )
			{
				if ( pTask->m_dRead.m_bInUse && pTask->m_dReadBuf.IsEmpty () )
				{
					sphLogDebugL ( "L canceling read" );
					pTask->m_pPayload->LeakRecvTo ( pTask->m_dReadBuf );
					CancelIoEx ( (HANDLE) pTask->m_ifd, &pTask->m_dRead );
				}

				if ( pTask->m_dWrite.m_bInUse && pTask->m_dWriteBuf.IsEmpty () && pTask->m_dOutIO.IsEmpty () )
				{
					sphLogDebugL ( "L canceling write" );
					pTask->m_pPayload->LeakSendTo ( pTask->m_dWriteBuf, pTask->m_dOutIO );
					CancelIoEx ( (HANDLE) pTask->m_ifd, &pTask->m_dWrite );
				}
			}

			return pTask->m_ifd==-1 ? -2 : 0;

			/*
			Hackers way to unbind from IOCP:

			Call NtSetInformationFile with the FileReplaceCompletionInformationenumerator value for 
			FileInformationClass and a pointer to a FILE_COMPLETION_INFORMATION structure for the FileInformation parameter. 
			In this structure, set the Port member to NULL (or nullptr, in C++) to disassociate the file from the port it's
			currently attached to (I guess if it isn't attached to any port, nothing would happen), or set Port to a valid
			HANDLE to another completion port to associate the file with that one instead.

			However it 1-st, require win >=8.1, and also invoke DDK stuff which is highly non-desirable. So, leave it 'as is'.
			*/
		}

		if ( !pTask->m_uIOActive )
		{
			sphLogDebugL ( "L Associate %d with iocp %d, %d events before", pTask->m_ifd, m_IOCP, m_iEvents );
			if ( !CreateIoCompletionPort ( (HANDLE) pTask->m_ifd, m_IOCP, (ULONG_PTR) pTask->m_ifd, 0 ) )
				sphLogDebugv ( "L Associate %d with port %d failed with error %d", pTask->m_ifd, m_IOCP, GetLastError () );
			return 2;
		}
		sphLogDebugL ( "L According to state, %d already associated with iocp %d, no action", pTask->m_ifd, m_IOCP );
		return 0;
	}

protected:

	// always return 0 (timeout) or 1 (since iocp returns per event, not the bigger group).
	inline int events_wait ( int64_t iTimeoutUS )
	{
		ULONG uReady = 0;
		DWORD uTimeout = ( iTimeoutUS>=0 ) ? ( iTimeoutUS/1000 ) : INFINITE;
		m_dReady.Resize ( m_iEvents+m_iCReserve ); // +1 since our signaler is not added as resident of the queue
		if ( !GetQueuedCompletionStatusEx ( m_IOCP, m_dReady.Begin (), m_dReady.GetLength (), &uReady, uTimeout, FALSE ) )
		{
			auto iErr = GetLastError ();
			if ( iErr==WAIT_TIMEOUT )
				return 0;

			sphLogDebugL ( "L GetQueuedCompletionStatusEx failed with error %d", iErr );
			return 0;
		}
		return uReady;
	}

	// returns task and also selects current event for all the functions below
	NetEvent_c GetEvent ( int iReady )
	{
		if ( iReady>=0 )
			return NetEvent_c ( &m_dReady[iReady] );
		assert (false);
		return NetEvent_c ( nullptr );
	}

#else
	int m_iEFD = -1;
	PollableEvent_t m_dSignaler;
	Task_t			m_dSignalerTask;

	inline void events_create ( int iSizeHint )
	{
		epoll_or_kqueue_create_impl ( iSizeHint );
		m_dReady.Reserve ( iSizeHint );

		// special event to wake up
		m_dSignalerTask.m_ifd = m_dSignaler.m_iPollablefd;
		// m_pPayload here used ONLY as store for pointer for comparing with &m_dSignaller,
		// NEVER called this way (since it NOT points to AgentConn_t instance)
		m_dSignalerTask.m_pPayload = ( AgentConn_t * ) &m_dSignaler;
		m_dSignalerTask.m_uIOChanged = Task_t::RO;

		sphLogDebugv( "Add internal signaller");
		events_change_io ( &m_dSignalerTask );
		sphLogDebugv ( "Internal signal action (for epoll/kqueue) added (%d), %p",
			m_dSignaler.m_iPollablefd, &m_dSignalerTask );

	}

	inline void fire_event ()
	{
		m_dSignaler.FireEvent ();
	}


#if POLLING_EPOLL
	CSphVector<epoll_event> m_dReady;

private:
	inline void epoll_or_kqueue_create_impl ( int iSizeHint )
	{
		m_iEFD = epoll_create ( iSizeHint ); // 1000 is dummy, see man
		if ( m_iEFD==-1 )
			sphDie ( "failed to create epoll main FD, errno=%d, %s", errno, strerrorm ( errno ) );
		m_dReady.Reserve ( m_iCReserve + iSizeHint );
		sphLogDebugv ( "epoll %d created", m_iEFD );
	}

	// apply changes in case of epoll
	int events_apply_task_changes ( Task_t * pTask )
	{
		auto iEvents = 0; // how many events we add/delete

		// if socket already closed (say, FATAL in action),
		// we don't need to unsubscribe events, but still need to return num of deleted events
		// to keep in health poller's input buffer
		bool bApply = pTask->m_ifd!=-1;

		bool bWrite = pTask->m_uIOChanged==Task_t::RW;
		bool bRead = pTask->m_uIOChanged!=Task_t::NO;

		int iOp = 0;
		epoll_event tEv = { 0 };
		tEv.data.ptr = pTask;

		// boring matrix of conditions...
		if ( !pTask->m_uIOChanged )
		{
			iOp = EPOLL_CTL_DEL;
			--iEvents;
			sphLogDebugL ( "L EPOLL_CTL_DEL(%d), %d+%d events", pTask->m_ifd, m_iEvents, iEvents );
		} else
		{
			tEv.events = ( bRead ? EPOLLIN : 0 ) | ( bWrite ? EPOLLOUT : 0 ) | ( ( pTask==&m_dSignalerTask ) ? 0 : EPOLLET );

			if ( !pTask->m_uIOActive )
			{
				iOp = EPOLL_CTL_ADD;
				++iEvents;
				sphLogDebugL ( "L EPOLL_CTL_ADD(%d) -> %x, %d+%d events", pTask->m_ifd, tEv.events, m_iEvents, iEvents );
			} else
			{
				iOp = EPOLL_CTL_MOD;
				sphLogDebugL ( "L EPOLL_CTL_MOD(%d) -> %x, %d+%d events", pTask->m_ifd, tEv.events, m_iEvents, iEvents );
			}
		}

		if ( bApply )
		{
			auto iRes = epoll_ctl ( m_iEFD, iOp, pTask->m_ifd, &tEv );
			if ( iRes==-1 )
				sphLogDebugL ( "L failed to perform epollctl for sock %d(%p), errno=%d, %s"
					  , pTask->m_ifd, pTask, errno, strerrorm ( errno ) );
		} else
			sphLogDebugL ( "L epoll_ctl not called since sock is closed" );
		return iEvents;
	}


protected:
	inline void events_destroy ()
	{
		sphLogDebugv ( "epoll %d closed", m_iEFD );
		SafeClose ( m_iEFD );
	}

	inline int events_wait ( int64_t iTimeoutUS )
	{
		m_dReady.Resize ( m_iEvents + m_iCReserve );
		int iTimeoutMS = iTimeoutUS<0 ? -1 : ( ( iTimeoutUS + 500 ) / 1000 );
		return epoll_wait ( m_iEFD, m_dReady.Begin (), m_dReady.GetLength (), iTimeoutMS );
	};

	// returns task and also selects current event for all the functions below
	NetEvent_c GetEvent ( int iReady )
	{
		if ( iReady>=0 )
			return NetEvent_c ( &m_dReady[iReady], &m_dSignalerTask );
		assert ( false );
		return NetEvent_c ( nullptr, &m_dSignalerTask );
	}

#elif POLLING_KQUEUE
	CSphVector<struct kevent> m_dReady;
	CSphVector<struct kevent> m_dScheduled; // prepared group of events
	struct kevent * m_pEntry = nullptr;

private:
	inline void epoll_or_kqueue_create_impl ( int iSizeHint )
	{
		m_iEFD = kqueue ();
		if ( m_iEFD==-1 )
			sphDie ( "failed to create kqueue main FD, errno=%d, %s", errno, strerrorm ( errno ) );

		sphLogDebugv ( "kqueue %d created", m_iEFD );
		m_dScheduled.Reserve ( iSizeHint * 2 );
		m_dReady.Reserve ( iSizeHint * 2 + m_iCReserve );
	}

	int events_apply_task_changes ( Task_t * pTask )
	{
		int iEvents = 0;
		bool bWrite = pTask->m_uIOChanged==Task_t::RW;
		bool bRead = pTask->m_uIOChanged!=Task_t::NO;
		bool bWasWrite = pTask->m_uIOActive==Task_t::RW;;
		bool bWasRead = ( pTask->m_uIOActive!=Task_t::NO);
		bool bApply = pTask->m_ifd!=-1;

		// boring combination matrix below
		if ( bRead && !bWasRead )
		{
			if ( bApply )
				EV_SET ( &m_dScheduled.Add (), pTask->m_ifd, EVFILT_READ, EV_ADD, 0, 0, pTask );
			++iEvents;
			sphLogDebugL ( "L EVFILT_READ, EV_ADD, %d (%d enqueued), %d in call", pTask->m_ifd, m_dScheduled.GetLength ()
						   , iEvents );
		}

		if ( bWrite && !bWasWrite )
		{
			if ( bApply )
				EV_SET ( &m_dScheduled.Add (), pTask->m_ifd, EVFILT_WRITE, EV_ADD, 0, 0, pTask );
			++iEvents;
			sphLogDebugL ( "L EVFILT_WRITE, EV_ADD, %d (%d enqueued), %d in call", pTask->m_ifd, m_dScheduled.GetLength ()
						   , iEvents );
		}

		if ( !bRead && bWasRead )
		{
			if ( bApply )
				EV_SET ( &m_dScheduled.Add (), pTask->m_ifd, EVFILT_READ, EV_DELETE, 0, 0, pTask );
			--iEvents;
			sphLogDebugL ( "L EVFILT_READ, EV_DELETE, %d (%d enqueued), %d in call", pTask->m_ifd
						   , m_dScheduled.GetLength ()
						   , iEvents );
		}

		if ( !bWrite && bWasWrite )
		{
			if ( bApply )
				EV_SET ( &m_dScheduled.Add (), pTask->m_ifd, EVFILT_WRITE, EV_DELETE, 0, 0, pTask );
			--iEvents;
			sphLogDebugL ( "L EVFILT_WRITE, EV_DELETE, %d (%d enqueued), %d in call", pTask->m_ifd
						   , m_dScheduled.GetLength ()
						   , iEvents );
		}
		return iEvents;
	}

protected:
	inline void events_destroy ()
	{
		sphLogDebugv ( "kqueue %d closed", m_iEFD );
		SafeClose ( m_iEFD );
	}

	inline int events_wait ( int64_t iTimeoutUS )
	{
		m_dReady.Resize ( m_iEvents + m_dScheduled.GetLength () + m_iCReserve );
		timespec ts;
		timespec * pts = nullptr;
		if ( iTimeoutUS>=0 )
		{
			ts.tv_sec = iTimeoutUS / 1000000;
			ts.tv_nsec = ( long ) ( iTimeoutUS - ts.tv_sec * 1000000 ) * 1000;
			pts = &ts;
		}
		// need positive timeout for communicate threads back and shutdown
		auto iRes = kevent ( m_iEFD, m_dScheduled.begin (), m_dScheduled.GetLength (), m_dReady.begin ()
								, m_dReady.GetLength (), pts );
		m_dScheduled.Reset();
		return iRes;

	};

	// returns task and also selects current event for all the functions below
	NetEvent_c GetEvent ( int iReady )
	{
		if ( iReady>=0 )
			return NetEvent_c ( &m_dReady[iReady], &m_dSignalerTask );
		assert ( false );
		return NetEvent_c ( nullptr, &m_dSignalerTask );
	}

#endif
#endif
};

/// Like ISphNetEvents, but most syscalls optimized out
class LazyNetEvents_c : ISphNoncopyable, protected NetEventsFlavour_c
{
	using VectorTask_c = CSphVector<Task_t*>;

	// stuff to transfer (enqueue) tasks
	VectorTask_c *	m_pEnqueuedTasks GUARDED_BY (m_dActiveLock) = nullptr; // ext. mt queue where we add tasks
	VectorTask_c	m_dInternalTasks; // internal queue where we add our tasks without mutex
	CSphMutex	m_dActiveLock;
	TimeoutQueue_c m_dTimeouts;
	SphThread_t m_dWorkingThread;
	int			m_iLastReportedErrno = -1;
	volatile int	m_iTickNo = 1;
	int64_t		m_iNextTimeoutUS = 0;

private:
	/// maps AgentConn_t -> Task_c for new/existing task
	static inline Task_t * CreateNewTask ( AgentConn_t * pConnection )
	{
		auto pTask = new Task_t;
		pTask->m_ifd = pTask->m_iStoredfd = pConnection->m_iSock;
		pTask->m_pPayload = pConnection;
		pConnection->m_pPollerTask = pTask;
		pConnection->AddRef ();
		sphLogDebugv ( "- CreateNewTask for (%p)->%p, ref=%d", pConnection, pTask, (int) pConnection->GetRefcount () );
		return pTask;
	}

	// Simply deletes the task, but some tricks exist:
	// 1. (general): keeping payload nesessary when we fire timeout: the task is not necessary anyway,
	// however timeout callback need to be called with still valid (if any) payload.
	// 2. (win specific): On windows, however, another trick is in game: timeout condition we get from
	// internal GetQueuedCompletionStatusEx function. At the same time overlapped ops (WSAsend or recv, 
	// or even both) are still in work, and so we need to keep the 'overlapped' structs alive for them.
	// So, we can't just delete the task in the case. Instead we invalidate it (set m_ifd=-1, nullify payload),
	// so that the next return from events_wait will recognize it and finally totally destroy the task for us.
	AgentConn_t * DeleteTask ( Task_t * pTask, bool bReleasePayload=true )
	{
		assert ( pTask );
		sphLogDebugL ( "L DeleteTask for %p, (conn %p, io %d), release=%d", pTask, pTask->m_pPayload, pTask->m_uIOActive, bReleasePayload );
		pTask->m_uIOChanged = 0;
		events_change_io ( pTask );
		auto pConnection = pTask->m_pPayload;
		pTask->m_pPayload = nullptr;

		// if payload already invoked in another task (remember, we process deffered action!)
		// we won't nullify it.
		if ( pConnection && pConnection->m_pPollerTask==pTask )
			pConnection->m_pPollerTask = nullptr;

#if USE_WINDOWS
		pTask->m_ifd = -1;
		pTask = nullptr; // save from delete below
#endif
		SafeDelete ( pTask );

		if ( bReleasePayload )
			SafeRelease ( pConnection );
		return pConnection;
	}

	/// Atomically move m-t queue to single-thread internal queue.
	VectorTask_c * PopQueue () REQUIRES ( LazyThread ) EXCLUDES ( m_dActiveLock )
	{
		// atomically get current vec; put zero instead.
		VectorTask_c * pReadyQueue = nullptr;
		{
			ScopedMutex_t tLock ( m_dActiveLock );
			pReadyQueue = m_pEnqueuedTasks;
			m_pEnqueuedTasks = nullptr;
		}
		return pReadyQueue;
	}


	void ProcessChanges ( Task_t * pTask )
	{
		sphLogDebugL ( "L ProcessChanges for %p, (conn %p) (%d->%d), tm=" INT64_FMT " sock=%d", pTask, pTask->m_pPayload,
			pTask->m_uIOActive, pTask->m_uIOChanged, pTask->m_iTimeoutTime, pTask->m_ifd );

		assert ( pTask->m_iTimeoutTimeUS!=0);

		if ( pTask->m_iPlannedTimeout<0 ) // process delete.
		{
			sphLogDebugL ( "L finally remove task %p", pTask );
			m_dTimeouts.Remove ( pTask );
			DeleteTask ( pTask );
			sphLogDebugL ( "%s", m_dTimeouts.DebugDump ( "L " ).cstr () );
			return;
		}

		// on enqueued tasks m_uIOChanged == 0 doesn't request unsubscribe, but means 'nope'.
		// (unsubscription, in turn, means 'delete' and planned by setting timeout=-1)
		if ( pTask->m_uIOChanged )
			events_change_io ( pTask );

		if ( pTask->m_iPlannedTimeout )
		{
			pTask->m_iTimeoutTimeUS = pTask->m_iPlannedTimeout;
			pTask->m_iPlannedTimeout = 0;
			m_dTimeouts.Change ( pTask );
			sphLogDebugL ( "L change/add timeout for %p, " INT64_FMT " (%d) is changed one", pTask, pTask->m_iTimeoutTime,
				( int ) ( pTask->m_iTimeoutTime - sphMicroTimer () ) );
			sphLogDebugL ( "%s", m_dTimeouts.DebugDump ( "L " ).cstr () );
		}
	}

	/// take current internal and external queues, parse it and enqueue changes.
	/// actualy 1 task can have only 1 action (another change will change very same task).
	void ProcessEnqueuedTasks () REQUIRES ( LazyThread )
	{
		sphLogDebugL ( "L ProcessEnqueuedTasks" );

		auto VARIABLE_IS_NOT_USED uStartLen = m_dInternalTasks.GetLength ();

		auto pExternalQueue = PopQueue ();
		if ( pExternalQueue )
			m_dInternalTasks.Append ( *pExternalQueue );
		SafeDelete ( pExternalQueue );

		auto VARIABLE_IS_NOT_USED uLastLen = m_dInternalTasks.GetLength ();
		m_dInternalTasks.Uniq ();

		if ( m_dInternalTasks.IsEmpty () )
		{
			sphLogDebugL ( "L No tasks in queue" );
			return;
		}
		sphLogDebugL ( "L starting processing %d internal events (originally %d, sparsed %d)", m_dInternalTasks.GetLength (), uStartLen, uLastLen );

		for ( auto * pTask : m_dInternalTasks )
		{
			sphLogDebugL ( "L Start processing task %p", pTask );
			ProcessChanges ( pTask );
			sphLogDebugL ( "L Finish processing task %p", pTask );
		}
		sphLogDebugL ( "L All events processed" );
		m_dInternalTasks.Reset ();
	}

	/// main event loop run in separate thread.
	void EventLoop () REQUIRES ( LazyThread )
	{
		while ( true )
			if ( !EventTick () )
				break;
	}

	/// abandon and release all tiemouted events.
	/// \return next active timeout (in uS), or -1 for infinite.
	bool HasTimeoutActions()
	{
		bool bHasTimeout = false;
		while ( !m_dTimeouts.IsEmpty () )
		{
			auto* pTask = ( Task_t* ) m_dTimeouts.Root ();
			assert ( pTask->m_iTimeoutTimeUS>0 );

			m_iNextTimeoutUS = pTask->m_iTimeoutTimeUS - sphMicroTimer ();
			if ( m_iNextTimeoutUS>0 )
				return bHasTimeout;

			bHasTimeout = true;

			sphLogDebugL ( "L timeout happens for %p task", pTask );
			m_dTimeouts.Pop ();

			// Delete task, adopt connection.
			// Invoke Timeoutcallback for it
			CSphRefcountedPtr<AgentConn_t> pKeepConn ( DeleteTask ( pTask, false ) );
			sphLogDebugL ( "%s", m_dTimeouts.DebugDump ( "L heap:" ).cstr () );
			if ( pKeepConn )
			{
				/*
				 * Timeout means that r/w actions for task might be still active.
				 * Suppose that timeout functor will unsibscribe socket from polling.
				 * However if right now something came to the socket, next call to poller might
				 * signal it, and we catch the events on the next round.
				 */
				sphLogDebugL ( "L timeout action started" );
				pKeepConn->TimeoutCallback ();
				sphLogDebugL ( "L timeout action finished" );
			}
		}
		m_iNextTimeoutUS = -1;
		return bHasTimeout; /// means 'infinite'
	}

	/// abandon and release all events (on shutdown)
	void AbortScheduled ()
	{
		while ( !m_dTimeouts.IsEmpty () )
		{
			auto pTask = ( Task_t* ) m_dTimeouts.Root ();
			m_dTimeouts.Pop ();
			CSphRefcountedPtr<AgentConn_t> pKeepConn ( DeleteTask ( pTask, false ) );
			if ( pKeepConn )
				pKeepConn->AbortCallback ();
		}
	}

	inline bool IsTickProcessed ( Task_t * pTask )
	{
		if ( !pTask )
			return false;
		return pTask->m_iTickProcessed==m_iTickNo;
	}

	/// one event cycle.
	/// \return false to stop event loop and exit.
	bool EventTick () REQUIRES ( LazyThread )
	{
		sphLogDebugL ( "L ---------------------------- EventTick(%d)", m_iTickNo );
		do
			ProcessEnqueuedTasks ();
		while ( HasTimeoutActions () );


		sphLogDebugL ( "L calculated timeout is " INT64_FMT " useconds", m_iNextTimeoutUS );

		auto iStarted = sphMicroTimer ();
		auto iEvents = events_wait ( m_iNextTimeoutUS );
		auto iWaited = sphMicroTimer() - iStarted;

#if USE_WINDOWS
		ProcessEnqueuedTasks (); // we have 'pushed' our iocp inside, if it is fired, the fire event is last
#endif

		// tick # allows to trace different events over one and same task.
		// Say, write action processing may initiate reading, or even
		// invalidate connection closing it and releasing.
		// If later in the same loop we have same task for another action, such changed state
		// may cause crash (say, if underlying connection is released and deleted).
		// With epoll we have only one task which may be both 'write' and 'read' state,
		// so it seems that just do one ELSE another should always work.
		// But on BSD we have separate event for read and another for write.
		// If one processed, no guarantee that another is not in the same resultset.
		// For this case we actualize tick # on processing and then compare it with current one.
		++m_iTickNo;
		if ( !m_iTickNo ) ++m_iTickNo; // skip 0

		if ( g_bShutdown )
		{
			AbortScheduled();
			sphLogDebugL ( "EventTick() exit because of shutdown=%d", g_bShutdown );
			return false;
		}

		if ( iEvents<0 )
		{
			int iErrno = sphSockGetErrno ();
			if ( m_iLastReportedErrno!=iErrno )
			{
				sphLogDebugL ( "L poller tick failed: %s", sphSockError ( iErrno ) );
				m_iLastReportedErrno = iErrno;
			}
			sphLogDebugL ( "L poller tick failed: %s", sphSockError ( iErrno ) );
			return true;
		}
		sphLogDebugL ( "L poller wait returned %d events from %d", iEvents, m_iEvents );
		m_dReady.Resize ( iEvents );

		/// we have some events to speak about...
		for ( int i = 0; i<iEvents; ++i )
		{
			auto tEvent = GetEvent (i);
			if ( tEvent.IsSignaler () )
			{
				sphLogDebugL ( "L internal event. Disposed" );
				continue;
			}

			Task_t * pTask = tEvent.GetTask ();

			if ( !pTask )
			{
#if USE_WINDOWS
				m_iEvents -= 2;
#endif
				continue;
			}
			else
				sphLogDebugL ( "L event action for task %p(%d), %d", pTask, pTask->m_ifd, tEvent.GetEvents () );

			// part of consequencing crash catching; m.b. not actual anymore if no warnings fired
			// (stuff supporting IsNotHere is not necessary also in case).
			if ( m_dTimeouts.IsNotHere ( pTask ) )
			{
				// sphWarning ( "phantom event detected! %p(%d, original %d), %d, closing", pTask, pTask->m_ifd, pTask->m_iStoredfd, tEvent.GetEvents () );
				continue;
			}

			bool bError = tEvent.IsError ();
			bool bEof = tEvent.IsEof ();
			if ( bError )
			{
				sphLogDebugL ( "L error happened" );
				if ( bEof )
				{
					sphLogDebugL ( "L assume that is eof, discard the error" );
					bError = false;
				}
			}

			auto pConn = pTask->m_pPayload;
			if ( pConn && pTask->m_uIOActive && !IsTickProcessed ( pTask ) )
			{
				if ( bError )
				{
					sphLogDebugL ( "L error action %p, waited " INT64_FMT, pTask, iWaited );
					pTask->m_iTickProcessed = m_iTickNo;
					pConn->ErrorCallback ( iWaited );
					sphLogDebugL ( "L error action %p completed", pTask );
				} else
				{
					if ( tEvent.IsWrite () )
					{
						if ( !bEof )
						{
							sphLogDebugL ( "L write action %p, waited " INT64_FMT ", transferred %d", pTask, iWaited, tEvent.BytesTransferred () );
							pTask->m_iTickProcessed = m_iTickNo;
							pConn->SendCallback ( iWaited, tEvent.BytesTransferred () );
							sphLogDebugL ( "L write action %p completed", pTask );
						} else
							sphLogDebugL ( "L write action avoid because of eof or same-generation tick", pTask );
					}

					if ( tEvent.IsRead () && !IsTickProcessed ( pTask ) )
					{
						sphLogDebugL ( "L read action %p, waited " INT64_FMT ", transferred %d", pTask, iWaited, tEvent.BytesTransferred () );
						pTask->m_iTickProcessed = m_iTickNo;
						pConn->RecvCallback ( iWaited, tEvent.BytesTransferred () );
						sphLogDebugL ( "L read action %p completed", pTask );
					}
				}
			}
		} // 'for' loop over ready events
		return true;
	}

	void AddToQueue ( Task_t * pTask, bool bInternal )
	{
		if ( bInternal )
		{
			sphLogDebugL ( "L AddToQueue, int=%d", m_dInternalTasks.GetLength () + 1 );
			m_dInternalTasks.Add ( pTask );
		} else
		{
			sphLogDebugL ( "- AddToQueue, ext=%d", m_pEnqueuedTasks ? m_pEnqueuedTasks->GetLength () + 1 : 1 );
			ScopedMutex_t tLock ( m_dActiveLock );
			if ( !m_pEnqueuedTasks )
				m_pEnqueuedTasks = new VectorTask_c;
			m_pEnqueuedTasks->Add ( pTask );
		}
	}

public:
	explicit LazyNetEvents_c ( int iSizeHint )
	{
		events_create ( iSizeHint );
		sphCrashThreadCreate ( &m_dWorkingThread, WorkerFunc, this, false, "AgentsPoller" );
	}

	~LazyNetEvents_c ()
	{
		sphLogDebug ( "~LazyNetEvents_c. Shutdown=%d", g_bShutdown );
		Fire();
		// might be crash - no need to hung waiting thread
		if ( g_bShutdown )
			sphThreadJoin ( &m_dWorkingThread );
		events_destroy();
	}

	/// New task (only applied to fresh connections; skip already enqueued)
	bool EnqueueNewTask ( AgentConn_t * pConnection, int64_t iTimeoutMS, BYTE uActivateIO )
	{
		if ( pConnection->m_pPollerTask )
			return false;

		Task_t * pTask = CreateNewTask ( pConnection );
		assert ( pTask );
		assert ( iTimeoutMS>0 );

		// check for same timeout as we have. Avoid dupes, if so.

		pTask->m_iPlannedTimeout = iTimeoutMS;
		if ( uActivateIO )
			pTask->m_uIOChanged = uActivateIO;

		// for win it is vitable important to apply changes immediately,
		// since iocp has to be enqueued before read/write op, not after!
		// Tomat: moved here to fix race between call from main thd from ScheduleDistrJobs and loop processing from ProcessEnqueuedTasks
#if USE_WINDOWS
		if ( uActivateIO )
			events_change_io ( pTask );
#endif

		sphLogDebugv ( "- %d EnqueueNewTask %p (%p) " INT64_FMT " Us, IO(%d->%d), sock %d", pConnection->m_iStoreTag, pTask, pConnection, iTimeoutMS, pTask->m_uIOActive, pTask->m_uIOChanged, pConnection->m_iSock );
		AddToQueue ( pTask, pConnection->InNetLoop () );

		return true;
	}

	void ChangeDeleteTask ( AgentConn_t * pConnection, int64_t iTimeoutUS )
	{
		auto pTask = ( Task_t * ) pConnection->m_pPollerTask;
		assert ( pTask );

		// check for same timeout as we have. Avoid dupes, if so.
		if ( !iTimeoutUS || pTask->m_iTimeoutTimeUS==iTimeoutUS )
			return;

		pTask->m_iPlannedTimeout = iTimeoutUS;

		// case of delete: pConn socket m.b. already closed and ==-1. Actualize it right now.
		if ( iTimeoutUS<0 )
		{
			pTask->m_ifd = pConnection->m_iSock;
			pConnection->m_pPollerTask = nullptr; // this will allow to create another task.
			sphLogDebugv ( "- %d Delete task (task %p), fd=%d (%d) " INT64_FMT "Us",
				pConnection->m_iStoreTag, pTask, pTask->m_ifd, pTask->m_iStoredfd, pTask->m_iTimeoutTimeUS );
		} else
			sphLogDebugv ( "- %d Change task (task %p), fd=%d (%d) " INT64_FMT "Us -> " INT64_FMT "Us",
				pConnection->m_iStoreTag, pTask, pTask->m_ifd, pTask->m_iStoredfd, pTask->m_iTimeoutTimeUS, iTimeoutUS );

		
		AddToQueue ( pTask, pConnection->InNetLoop () );
	}

	void DisableWrite ( AgentConn_t * pConnection )
	{
		auto pTask = ( Task_t * ) pConnection->m_pPollerTask;
		assert ( pTask );

		if ( Task_t::RO!=pTask->m_uIOActive )
		{
			pTask->m_uIOChanged = Task_t::RO;
			sphLogDebugv ( "- %d DisableWrite enqueueing (task %p) (%d->%d), innet=%d", pConnection->m_iStoreTag, pTask,
						   pTask->m_uIOActive, pTask->m_uIOChanged, pConnection->InNetLoop());

			AddToQueue ( pTask, pConnection->InNetLoop () );
		}
	}

	/// then signal the poller.
	void Fire ()
	{
		sphLogDebugL ("L Fire an event invoked");
		fire_event ();
	}

private:
	static void WorkerFunc ( void * pArg ) REQUIRES ( !LazyThread )
	{
		ScopedRole_c thLazy ( LazyThread );
		auto * pThis = ( LazyNetEvents_c * ) pArg;
		sphLogDebugL ( "L RemoteAgentsPoller_t::WorkerFunc started" );
		pThis->EventLoop ();
	}
};

//! Get static (singletone) instance of lazy poller
//! C++11 guarantees it to be mt-safe (magic-static).
LazyNetEvents_c & LazyPoller ()
{
	static LazyNetEvents_c dEvents ( 1000 );
	return dEvents;
}

//! Add or change task for poller.
void AgentConn_t::LazyTask ( int64_t iTimeoutMS, bool bHardTimeout, BYTE uActivateIO )
{
	assert ( iTimeoutMS>0 );

	m_bNeedKick = !InNetLoop();
	m_eTimeoutKind = bHardTimeout ? TIMEOUT_HARD : TIMEOUT_RETRY;
	LazyPoller ().EnqueueNewTask ( this, iTimeoutMS, uActivateIO );
}

void AgentConn_t::LazyDeleteOrChange ( int64_t iTimeoutMS )
{
	// skip del/change for not scheduled conns
	if ( !m_pPollerTask )
		return;

	LazyPoller ().ChangeDeleteTask ( this, iTimeoutMS );
}


void AgentConn_t::DisableWrite ()
{
	// skip del/change for not scheduled conns
	if ( !m_pPollerTask )
		return;

	LazyPoller ().DisableWrite ( this );
}

void FirePoller ()
{
	LazyPoller ().Fire ();
}


class CRemoteAgentsObserver : public RemoteAgentsObserver_i
{
protected:
	CSphAtomic m_iSucceeded;	//< num of tasks finished successfully
	CSphAtomic m_iFinished;		//< num of tasks finished.
	CSphAtomic m_iTasks;		//< total num of tasks
	CoroEvent_c m_tChanged;		//< the signaller

public:
	void FeedTask ( bool bAdd ) final
	{
		if ( bAdd )
			++m_iTasks;
		else
			--m_iTasks;
	}

	// check that there are no works to do
	bool IsDone () const final
	{
		if ( m_iFinished > m_iTasks )
			sphWarning ( "Orphaned chain detected (expected %d, got %d)", (int)m_iTasks, (int)m_iFinished );

		return m_iFinished>=m_iTasks;
	}

	// block execution until all tasks are finished
	void Finish () final
	{
		while (!IsDone())
			WaitChanges ();
	}

	inline long GetSucceeded() const final
	{
		return m_iSucceeded;
	}

	inline long GetFinished () const final
	{
		return m_iFinished;
	}

public:
	void Report ( bool bSuccess ) final
	{
		if ( bSuccess )
			++m_iSucceeded;
		++m_iFinished;
		m_tChanged.SetEvent ();
	}

	// block execution while some works finished
	void WaitChanges () final
	{
		m_tChanged.WaitEvent ();
	}
};

// coro-based lock-free observer:
// WaitChanges check boolean signal and either returns immediately, either yield.
// reporter either just set signal, either reschedule waitchanges continuation
RemoteAgentsObserver_i * GetObserver ()
{
	return new CRemoteAgentsObserver;
}

/// check if a non-blocked socket is still connected
bool sphNBSockEof ( int iSock )
{
	if ( iSock<0 )
		return true;

	char cBuf;
	// since socket is non-blocked, ::recv will not block anyway
	int iRes = ::recv ( iSock, &cBuf, sizeof ( cBuf ), MSG_PEEK );
	if ( (!iRes) || (iRes<0
		&& sphSockGetErrno ()!=EWOULDBLOCK
		&& sphSockGetErrno ()!=EAGAIN ))
		return true;
	return false;
}


// in case of epoll/kqueue the full set of polled sockets are stored
// in a cache inside kernel, so once added, we can't iterate over all of the items.
// So, we store them in linked list for that purpose.

struct NetListedNode_t : public ListedData_t
{
	explicit NetListedNode_t ( const NetPollEvent_t * pEvent )
		: ListedData_t ( pEvent ) {}

	NetPollEvent_t * GetEvent() const
	{
		return ( NetPollEvent_t * ) m_pData;
	}
};

// store and iterate over the list of items
struct EventsList_t
{
	List_t m_tWorkList;

	NetListedNode_t * AddNewEvent ( const NetPollEvent_t * pEvent )
	{
		assert ( pEvent );
		auto * pIntData = new NetListedNode_t ( pEvent );
		pEvent->m_tBack.pPtr = pIntData;
		m_tWorkList.Add ( pIntData );
		return pIntData;
	}

	void RemoveElem ( ListNode_t * pElem )
	{
		m_tWorkList.Remove ( pElem );
		SafeDelete ( pElem );
	}

	void RemoveNetEvent ( NetPollEvent_t * pEvent )
	{
		auto* pList = ( NetListedNode_t* ) pEvent->m_tBack.pPtr;
		pEvent->m_tBack.pPtr = nullptr;
		RemoveElem ( pList );
	}

	virtual ~EventsList_t ()
	{
		while ( m_tWorkList.GetLength() )
		{
			auto * pItem = (NetListedNode_t *)m_tWorkList.Begin();
			m_tWorkList.Remove ( pItem );
			SafeDelete( pItem );
		}
	}
};

class TimeoutEvents_c
{
	TimeoutQueue_c	m_dTimeouts;

public:
	void AddOrChangeTimeout ( EnqueuedTimeout_t * pEvent )
	{
		if ( pEvent->m_iTimeoutTimeUS>=0 )
			m_dTimeouts.Change ( pEvent );
	}

	void RemoveTimeout ( EnqueuedTimeout_t * pEvent )
	{
		m_dTimeouts.Remove ( pEvent );
	}

	int GetNextTimeoutMs ()
	{
		int64_t iNextTimeoutUS = -1;
		while (!m_dTimeouts.IsEmpty ())
		{
			auto * pNetEvent = (EnqueuedTimeout_t *) m_dTimeouts.Root ();
			assert ( pNetEvent->m_iTimeoutTimeUS>0 );
			iNextTimeoutUS = pNetEvent->m_iTimeoutTimeUS-sphMicroTimer ();
			if ( iNextTimeoutUS>999 )
				break;

			m_dTimeouts.Pop ();
		}
		return (iNextTimeoutUS>999) ? int ( iNextTimeoutUS / 1000 ) : -1;
	}
};


#if ( NETPOLL_TYPE==NETPOLL_EPOLL )
class NetPooller_c::Impl_c : public EventsList_t, public TimeoutEvents_c
{
	friend class NetPooller_c;
	CSphVector<epoll_event>		m_dReady;
	int							m_iReady = 0;
	int							m_iLastReportedErrno = -1;
	int							m_iEFD;
	friend class NetPollReadyIterator_c;

public:
	explicit Impl_c ( int iSizeHint )
	{
		m_iEFD = epoll_create ( iSizeHint ); // 1000 is dummy, see man
		if ( m_iEFD==-1 )
			sphDie ( "failed to create epoll main FD, errno=%d, %s", errno, strerrorm ( errno ) );

		sphLogDebugv ( "epoll %d created", m_iEFD );
		m_dReady.Reserve ( iSizeHint );
	}

	~Impl_c () final
	{
		sphLogDebugv ( "epoll %d closed", m_iEFD );
		SafeClose ( m_iEFD );
	}

	void SetupEvent ( NetPollEvent_t * pData )
	{
		assert ( pData && pData->m_iSock >=0 );
		auto uEvents = pData->m_uNetEvents; // shortcut
		assert (( uEvents & NetPollEvent_t::WRITE )!=0 || ( uEvents & NetPollEvent_t::READ )!=0 );

		int iOp = EPOLL_CTL_MOD;
		auto * pIntData = pData->m_tBack.pPtr;
		if ( !pIntData ) // new event, need to add
		{
			iOp = EPOLL_CTL_ADD;
			pIntData = AddNewEvent ( pData );
		}

		AddOrChangeTimeout ( pData );

		epoll_event tEv;
		tEv.data.ptr = pIntData;
		tEv.events = (( uEvents & NetPollEvent_t::READ )!=0 ) ? EPOLLIN : EPOLLOUT;
		if ( uEvents & NetPollEvent_t::ONCE )
			tEv.events |= EPOLLONESHOT | EPOLLET;

		sphLogDebugv ("%p epoll %d setup, ev=0x%u, op=%d, sock=%d", pData, m_iEFD, tEv.events, iOp, pData->m_iSock);

		int iRes = epoll_ctl ( m_iEFD, iOp, pData->m_iSock, &tEv );
		if ( iRes==-1 )
			sphWarning ( "failed to setup epoll event for sock %d, errno=%d, %s",
					pData->m_iSock, errno, strerrorm ( errno ) );
	}

	void Wait ( int timeoutMs )
	{
		if ( !m_tWorkList.GetLength () )
			return;

		if ( timeoutMs==WAIT_UNTIL_TIMEOUT )
			timeoutMs = GetNextTimeoutMs ();

		m_dReady.Resize ( m_tWorkList.GetLength () );
		// need positive timeout for communicate threads back and shutdown
		m_iReady = epoll_wait ( m_iEFD, m_dReady.Begin (), m_dReady.GetLength (), timeoutMs );

		if ( m_iReady>=0 )
			return;

		int iErrno = sphSockGetErrno ();
		// common recoverable errors
		if ( iErrno==EINTR || iErrno==EAGAIN || iErrno==EWOULDBLOCK )
			return;

		if ( m_iLastReportedErrno!=iErrno )
		{
			sphWarning ( "epoll tick failed: %s", sphSockError ( iErrno ) );
			m_iLastReportedErrno = iErrno;
		}
	}

	void ForAll ( std::function<void ( NetPollEvent_t * )> && fnAction )
	{
		for ( auto& tElem : m_tWorkList )
		{
			auto pNode = (NetListedNode_t *) &tElem;
			if ( pNode->GetEvent () )
				fnAction ( pNode->GetEvent () );
			else
				RemoveElem ( pNode );
		}
	}

	int GetNumOfReady () const
	{
		return m_iReady;
	}

	void DeactivateTimer ( NetPollEvent_t * pEvent )
	{
		assert ( pEvent );
		RemoveTimeout ( pEvent );
	}

	// that is called from another thread
	void Unlink ( NetPollEvent_t * pEvent, bool bWillClose )
	{
		assert ( pEvent );
		assert ( pEvent->m_iTimeoutIdx == -1 );
		auto * pIntData = (ListedData_t *) pEvent->m_tBack.pPtr;
		if ( !pIntData )
			return;
		pIntData->m_pData = nullptr;
		pEvent->m_tBack.pPtr = nullptr;

		if ( !bWillClose ) // for persistent we also delete socket from kqueue
		{
			epoll_event tEv;
			epoll_ctl ( m_iEFD, EPOLL_CTL_DEL, pEvent->m_iSock, &tEv );
		}
		// EventsList_t::RemoveEvent ( pEvent ); // that is not mt-safe, will deffer delete from net loop
	}

	void RemoveEvent ( NetPollEvent_t * pEvent )
	{
		assert ( pEvent );

		sphLogDebugv ( "%p epoll remove, ev=0x%u, sock=%d",
					   pEvent->m_tBack.pPtr, pEvent->m_uNetEvents, pEvent->m_iSock );
		epoll_event tEv;
		int iRes = epoll_ctl ( m_iEFD, EPOLL_CTL_DEL, pEvent->m_iSock, &tEv );

		// might be already closed by worker from thread pool
		if ( iRes==-1 )
			sphLogDebugv ( "failed to remove epoll event for sock %d(%p), errno=%d, %s",
					pEvent->m_iSock, pEvent->m_tBack.pPtr, errno, strerrorm ( errno ) );

		RemoveTimeout ( pEvent );
		EventsList_t::RemoveNetEvent( pEvent );
	}

	void Shutdown()
	{
		SafeCloseSocket ( m_iEFD );
		while ( m_tWorkList.GetLength () )
		{
			auto * pItem = (NetListedNode_t *) m_tWorkList.Begin ();
			m_tWorkList.Remove ( pItem );
			NetPollEvent_t * pEvent = pItem->GetEvent();
			if ( pEvent )
			{
				pEvent->m_tBack.pPtr = nullptr;
				pEvent->m_iTimeoutIdx = -1;
			}
			SafeDelete( pItem );
		}
	}
};

NetPollEvent_t & NetPollReadyIterator_c::operator* ()
{
	NetPollEvent_t * pNode = nullptr;
	auto * pOwner = m_pOwner->m_pImpl;
	const epoll_event & tEv = pOwner->m_dReady[m_iIterEv];
	pNode = ((NetListedNode_t *) ( tEv.data.ptr ))->GetEvent ();

	pNode->m_uNetEvents = 0;
	if ( tEv.events & EPOLLIN )
		pNode->m_uNetEvents |= NetPollEvent_t::READ;
	if ( tEv.events & EPOLLOUT )
		pNode->m_uNetEvents |= NetPollEvent_t::WRITE;
	if ( tEv.events & EPOLLHUP )
		pNode->m_uNetEvents |= NetPollEvent_t::HUP;
	if ( tEv.events & EPOLLERR )
		pNode->m_uNetEvents |= NetPollEvent_t::ERR;
//	if ( tEv.events & EPOLLPRI )
//		pNode->m_uNetEvents |= NetPollEvent_t::PRI;

	return *pNode;
};

NetPollReadyIterator_c & NetPollReadyIterator_c::operator++ ()
{
	++m_iIterEv;
	return *this;
}

bool NetPollReadyIterator_c::operator!= ( const NetPollReadyIterator_c & rhs ) const
{
	auto* pOwner = m_pOwner->m_pImpl;
	return rhs.m_pOwner || m_iIterEv<pOwner->m_iReady;
}

#endif

#if ( NETPOLL_TYPE == NETPOLL_KQUEUE )

class NetPooller_c::Impl_c final : public EventsList_t, public TimeoutEvents_c
{
	friend class NetPooller_c;
	friend class NetPollReadyIterator_c;

	CSphVector<struct kevent>	m_dReady;
	int							m_iReady = 0;
	int							m_iLastReportedErrno = -1;
	int							m_iKQ;

public:
	explicit Impl_c ( int iSizeHint )
	{
		m_iKQ = kqueue ();
		if ( m_iKQ==-1 )
			sphDie ( "failed to create kqueue main FD, errno=%d, %s", errno, strerrorm ( errno ) );

		sphLogDebugv ( "kqueue %d created", m_iKQ );
		m_dReady.Reserve ( iSizeHint );
	}

	~Impl_c () final
	{
		sphLogDebugv ( "kqueue %d closed", m_iKQ );
		SafeCloseSocket ( m_iKQ );
	}

	void SetupEvent ( NetPollEvent_t * pData )
	{
		assert ( pData && pData->m_iSock>=0 );
		auto uEvents = pData->m_uNetEvents; // shortcut
		assert ( ( uEvents & NetPollEvent_t::WRITE )!=0 || ( uEvents & NetPollEvent_t::READ )!=0 );

		auto * pIntData = pData->m_tBack.pPtr;
		if ( !pIntData ) // new event, need to add
			pIntData = AddNewEvent ( pData );

		AddOrChangeTimeout ( pData );
		DWORD uFlags = ( uEvents & NetPollEvent_t::ONCE ) ? EV_ADD | EV_ONESHOT | EV_CLEAR : EV_ADD;

		struct kevent tEv;
		EV_SET ( &tEv, pData->m_iSock, ( uEvents&NetPollEvent_t::READ ? EVFILT_READ : EVFILT_WRITE),
				uFlags, 0, 0, pIntData );

		sphLogDebugv ( "%p kqueue %d setup, ev=%d, fl=%d sock=%d", pData, m_iKQ, tEv.filter, uFlags, pData->m_iSock );

		int iRes = kevent (m_iKQ, &tEv, 1, nullptr, 0, nullptr);
		if ( iRes==-1 )
			sphWarning ( "failed to setup kqueue event for sock %d, errno=%d, %s",
					pData->m_iSock, errno, strerrorm ( errno ) );
	}

	void Wait ( int timeoutMs )
	{
		if ( !m_tWorkList.GetLength ())
			return;

		if ( timeoutMs==WAIT_UNTIL_TIMEOUT )
			timeoutMs = GetNextTimeoutMs ();

		m_dReady.Resize ( m_tWorkList.GetLength () );

		timespec ts;
		timespec *pts = nullptr;
		if ( timeoutMs>=0 )
		{
			ts.tv_sec = timeoutMs/1000;
			ts.tv_nsec = (long)(timeoutMs-ts.tv_sec*1000)*1000000;
			pts = &ts;
		}
		// need positive timeout for communicate threads back and shutdown
		m_iReady = kevent (m_iKQ, nullptr, 0, m_dReady.begin(), m_dReady.GetLength(), pts);

		if ( m_iReady>=0 )
			return;

		int iErrno = sphSockGetErrno ();
		// common recoverable errors
		if ( iErrno==EINTR || iErrno==EAGAIN || iErrno==EWOULDBLOCK )
			return;

		if ( m_iLastReportedErrno!=iErrno )
		{
			sphWarning ( "kqueue tick failed: %s", sphSockError ( iErrno ) );
			m_iLastReportedErrno = iErrno;
		}
	}

	void ForAll ( std::function<void ( NetPollEvent_t * )> && fnAction )
	{
		for ( auto & tElem : m_tWorkList )
		{
			auto pNode = (NetListedNode_t *) &tElem;
			if ( pNode->GetEvent () )
				fnAction ( pNode->GetEvent () );
			else
				RemoveElem ( pNode );
		}
	}

	int GetNumOfReady () const
	{
		return m_iReady;
	}

	void DeactivateTimer ( NetPollEvent_t * pEvent )
	{
		assert ( pEvent );
		RemoveTimeout ( pEvent );
	}


	void RemoveEvent ( NetPollEvent_t * pEvent )
	{
		assert ( pEvent );

		sphLogDebugv ( "%p kqueue remove, ev=%u, sock=%d",
				pEvent->m_tBack.pPtr, pEvent->m_uNetEvents, pEvent->m_iSock );
		struct kevent tEv;
		EV_SET( &tEv, pEvent->m_iSock,
			( ( pEvent->m_uNetEvents & NetPollEvent_t::READ ) ? EVFILT_READ : EVFILT_WRITE ),
			EV_DELETE | EV_CLEAR, 0, 0, nullptr );
		int iRes = kevent ( m_iKQ, &tEv, 1, nullptr, 0, nullptr );

		// might be already closed by worker from thread pool
		if ( iRes==-1 )
			sphLogDebugv ( "failed to remove kqueue event for sock %d(%p), errno=%d, %s",
					pEvent->m_iSock, pEvent->m_tBack.pPtr, errno, strerrorm (errno));

		RemoveTimeout ( pEvent );
		RemoveNetEvent ( pEvent );
	}

	// that is called from another thread
	// unlink pEvent from kqueue, so that any further events on the socket
	// will not cause any processing. Also, if socket will not be closed promptly (i.e., persistent)
	// remove it totally from kqueue; we don't want to poll it more.
	void Unlink ( NetPollEvent_t * pEvent, bool bWillClose ) const
	{
		assert ( pEvent );
		auto * pIntData = (ListedData_t *) pEvent->m_tBack.pPtr;
		if ( !pIntData )
			return;
		pIntData->m_pData = nullptr;
		pEvent->m_tBack.pPtr = nullptr;

		if ( !bWillClose ) // for persistent we also delete socket from kqueue
		{
			struct kevent tEv;
			EV_SET( &tEv, pEvent->m_iSock,
				( ( pEvent->m_uNetEvents & NetPollEvent_t::READ ) ? EVFILT_READ : EVFILT_WRITE ),
				EV_DELETE | EV_CLEAR, 0, 0, nullptr );
			kevent ( m_iKQ, &tEv, 1, nullptr, 0, nullptr );
		}
		// EventsList_t::RemoveEvent ( pEvent ); // that is not mt-safe, will deffer delete from net loop
	}


	void Shutdown()
	{
		SafeCloseSocket ( m_iKQ );
		while ( m_tWorkList.GetLength () )
		{
			auto * pItem = (NetListedNode_t *) m_tWorkList.Begin ();
			m_tWorkList.Remove ( pItem );
			NetPollEvent_t * pEvent = pItem->GetEvent();
			if ( pEvent )
			{
				pEvent->m_tBack.pPtr = nullptr;
				pEvent->m_iTimeoutIdx = -1;
			}
			SafeDelete( pItem );
		}
	}
};

NetPollEvent_t & NetPollReadyIterator_c::operator* ()
{
	NetPollEvent_t * pNode = nullptr;
	auto * pOwner = m_pOwner->m_pImpl;
	const struct kevent & tEv = pOwner->m_dReady[m_iIterEv];
	pNode = ((NetListedNode_t *) ( tEv.udata ))->GetEvent ();
	assert ( pNode && "deleted event recognized");
	pNode->m_uNetEvents = 0;
	if ( tEv.filter==EVFILT_READ )
		pNode->m_uNetEvents |= NetPollEvent_t::READ;
	if ( tEv.filter==EVFILT_WRITE )
		pNode->m_uNetEvents |= NetPollEvent_t::WRITE;
	if ( tEv.flags & EV_ERROR )
		pNode->m_uNetEvents |= NetPollEvent_t::ERR;
	if ( tEv.flags & EV_EOF )
		pNode->m_uNetEvents |= NetPollEvent_t::HUP;
//	if ( tEv.flags & EV_OOBAND )
//		pNode->m_uNetEvents |= NetPollEvent_t::PRI;

	return *pNode;
};

NetPollReadyIterator_c & NetPollReadyIterator_c::operator++ ()
{
	++m_iIterEv;
	return *this;
}

bool NetPollReadyIterator_c::operator!= ( const NetPollReadyIterator_c & rhs ) const
{
	auto * pOwner = m_pOwner->m_pImpl;
	return rhs.m_pOwner || m_iIterEv<pOwner->m_iReady;
}

#endif

// fixme! check if one general version is enough for all flavours.
bool TimeoutReached ( int64_t tmLeft, int64_t tmNow )
{
	if (tmNow<0)
		tmNow = sphMicroTimer ();

	return ( tmLeft-1000 )<tmNow;
}

#if ( NETPOLL_TYPE == NETPOLL_POLL )
class NetPooller_c::Impl_c : public TimeoutEvents_c
{
	friend class NetPooller_c;
	friend class NetPollReadyIterator_c;

	CSphVector<NetPollEvent_t *>	m_dWork;
	CSphVector<pollfd>	m_dEvents;
	CSphVector<int>		m_dDefferedRemove;
	int					m_iReady = 0;
	int					m_iLastReportedErrno = -1;

	/*
	void DumpEvents(const char* szLegend) {
		StringBuilder_c ss;
		ss << szLegend << " " << m_dEvents.GetLength () << " events: ";
		ss.StartBlock ( ", " );
		for ( const auto& tElem : m_dEvents )
			ss.Sprintf ( "%d(%d)", tElem.fd, tElem.revents );
		sphLogDebugv ( "%s", ss.cstr () );
	} */

	// since remove() may be call from inside iteration,
	// let's keep kill-list and apply later instead of immediate delete
	void RemoveDeffered ()
	{
		if ( m_dDefferedRemove.IsEmpty () )
			return;

		m_dDefferedRemove.Uniq ();
		// need to walk removal index list backward to remove items from m_dWork that just passed by
		m_dDefferedRemove.RSort ();

		//DumpEvents ( "Before remove" );

		for ( int iToRemove : m_dDefferedRemove )
		{
			auto pEvent=m_dWork[iToRemove];
			RemoveTimeout ( pEvent );
			m_dEvents.RemoveFast ( iToRemove );
			m_dWork.RemoveFast ( iToRemove );
			sphLogDebugvv ( "Removed deffered %d", iToRemove );
			// restore index
			if ( iToRemove<m_dWork.GetLength() )
				m_dWork[iToRemove]->m_tBack.iIdx = iToRemove;
		}
		m_dDefferedRemove.Resize (0);
		//DumpEvents ( "After remove" );
	}

public:
	explicit Impl_c ( int iSizeHint )
	{
		m_dWork.Reserve ( iSizeHint );
		m_dEvents.Reserve ( iSizeHint );
	}

	void SetupEvent ( NetPollEvent_t * pData )
	{
		assert ( pData && pData->m_iSock>=0 );
		auto uEvents = pData->m_uNetEvents; // shortcut
		assert ( ( uEvents & ( NetPollEvent_t::WRITE | NetPollEvent_t::READ ) ) !=0 );
		RemoveDeffered ();

		AddOrChangeTimeout ( pData );

		assert ( m_dEvents.GetLength()==m_dWork.GetLength() );

		pollfd* pEv = nullptr;
		auto iIdx = pData->m_tBack.iIdx;
		auto uNetEvents = ( ( uEvents & NetPollEvent_t::READ ) != 0 ) ? POLLIN : POLLOUT;
		if ( iIdx>=0 && iIdx<m_dWork.GetLength () && !m_dWork[iIdx] ) // was already enqueued
		{
			pEv = &m_dEvents[iIdx];
			m_dWork[iIdx] = pData;
			sphLogDebugvv ( "SetupEvent [%d] old %d events %d", pData->m_tBack.iIdx, pData->m_iSock, uNetEvents );
		} else
		{
			pData->m_tBack.iIdx = m_dWork.GetLength ();
			m_dWork.Add ( pData );
			pEv = &m_dEvents.Add ();
			sphLogDebugvv ( "SetupEvent [%d] new %d events %d", pData->m_tBack.iIdx, pData->m_iSock, uNetEvents );
		}

		pEv->fd = pData->m_iSock;
		pEv->events = ( ( uEvents & NetPollEvent_t::READ )!=0 ) ? POLLIN : POLLOUT;
		sphLogDebugvv ( "SetupEvent [%d] for %d events %d", pData->m_tBack.iIdx, pData->m_iSock, pEv->events );
	}

	void Wait ( int timeoutMs )
	{
		m_iReady = 0;
		RemoveDeffered ();
		if ( m_dEvents.IsEmpty() )
			return;

		// need positive timeout for communicate threads back and shutdown
		if ( timeoutMs==WAIT_UNTIL_TIMEOUT )
			timeoutMs = GetNextTimeoutMs ();

		m_dEvents.Apply ( [] ( pollfd & dEvent ) { dEvent.revents = 0; } );
		m_iReady = ::poll ( m_dEvents.Begin (), m_dEvents.GetLength (), timeoutMs );

		if ( m_iReady>=0 )
		{
			sphLogDebugvv ( "Wait returned %d events", m_iReady );
			//DumpEvents ( "Wait" );
			return;
		}

		int iErrno = sphSockGetErrno ();
		sphLogDebugvv ( "Wait returned %d events with %d after-error", m_iReady, iErrno );
		// common recoverable errors
		if ( iErrno==EINTR || iErrno==EAGAIN || iErrno==EWOULDBLOCK )
			return;

		if ( m_iLastReportedErrno!=iErrno )
		{
			sphWarning ( "poll tick failed: %s", sphSockError ( iErrno ) );
			m_iLastReportedErrno = iErrno;
		}
	}

	void ForAll ( std::function<void ( NetPollEvent_t * )> && fnAction )
	{
		RemoveDeffered ();
		ARRAY_FOREACH ( i, m_dWork )
		{
			auto pData = m_dWork[i];
			if ( pData && pData->m_tBack.iIdx >= 0 )
				fnAction ( pData );
			else
			{
				sphLogDebugvv ( "Remove unlinked %d via 'all' iteration", i );
				m_dDefferedRemove.Add ( i ); // unlinked event
			}
		}
	}

	int GetNumOfReady () const
	{
		return m_iReady;
	}

	void DeactivateTimer ( NetPollEvent_t * pEvent )
	{
		assert ( pEvent );
		RemoveTimeout ( pEvent );
	}

	// that is called from another thread
	static void Unlink ( NetPollEvent_t * pEvent, bool )
	{
		sphLogDebugvv ( "Unlink %d, fd=%d", pEvent->m_tBack.iIdx, pEvent->m_iSock );
		assert ( pEvent );
		assert ( pEvent->m_iTimeoutIdx==-1 );
		// assert ( pEvent->m_uNetEvents & NetPollEvent_t::ONCE ); // ensure that is oneshot

		pEvent->m_tBack.iIdx = -1;
		// EventsList_t::RemoveEvent ( pEvent ); // that is not mt-safe, will deffer delete from net loop
	}

	void RemoveEvent ( NetPollEvent_t * pEvent )
	{
		assert ( pEvent->m_tBack.iIdx>=0 && pEvent->m_tBack.iIdx<m_dEvents.GetLength ());
		assert ( m_dEvents.GetLength ()==m_dWork.GetLength ());

		RemoveTimeout ( pEvent );
		m_dDefferedRemove.Add ( pEvent->m_tBack.iIdx );
		sphLogDebugvv ( "RemoveEvent for %d, fd=%d", pEvent->m_tBack.iIdx, pEvent->m_iSock );
	}

	void Shutdown ()
	{
		m_dWork.Apply ( [] ( NetPollEvent_t * pEvent)
		{
			pEvent->m_tBack.iIdx = -1;
			pEvent->m_iTimeoutIdx = -1;
		});
		m_dWork.Reset();
		m_dEvents.Reset ();
		m_dDefferedRemove.Reset();
	}
};


NetPollEvent_t & NetPollReadyIterator_c::operator* ()
{
	auto * pOwner = m_pOwner->m_pImpl;
	NetPollEvent_t * pNode = pOwner->m_dWork[m_iIterEv];
	pollfd & tEv = pOwner->m_dEvents[m_iIterEv];

	sphLogDebugvv ( "[%d] tEv.revents = %d for %d(%d)", m_iIterEv, tEv.revents, pNode->m_iSock, tEv.fd );

	if (pNode->m_uNetEvents & NetPollEvent_t::ONCE)
	{
		tEv.fd = -tEv.fd;
		pOwner->m_dWork[m_iIterEv] = nullptr;
	}

	pNode->m_uNetEvents &= ~(NetPollEvent_t::ONCE);
	if ( tEv.revents & POLLIN )
		pNode->m_uNetEvents |= NetPollEvent_t::READ;
	if ( tEv.revents & POLLOUT )
		pNode->m_uNetEvents |= NetPollEvent_t::WRITE;
	if ( tEv.revents & POLLHUP )
		pNode->m_uNetEvents |= NetPollEvent_t::HUP;
	if ( tEv.revents & POLLERR )
		pNode->m_uNetEvents |= NetPollEvent_t::ERR;

	return *pNode;
}

NetPollReadyIterator_c & NetPollReadyIterator_c::operator++ ()
{
	auto * pOwner = m_pOwner->m_pImpl;
	while (true) {
		++m_iIterEv;
		if ( m_iIterEv>=pOwner->m_dEvents.GetLength ())
			return *this;

		pollfd& tEv = pOwner->m_dEvents[m_iIterEv];

		if ( tEv.fd>=0 && tEv.revents!=0 && tEv.revents!=POLLNVAL )
		{
			sphLogDebugvv ( "operator++ on m_iIterEv as matched %d and %d", tEv.fd, tEv.revents );
			return *this;
		}
	}
}

bool NetPollReadyIterator_c::operator!= ( const NetPollReadyIterator_c & rhs ) const
{
	auto * pOwner = m_pOwner->m_pImpl;
	return rhs.m_pOwner || m_iIterEv<pOwner->m_dEvents.GetLength ();
}
#endif


NetPooller_c::NetPooller_c ( int isizeHint )
		: m_pImpl ( new Impl_c ( isizeHint ) )
{}

NetPooller_c::~NetPooller_c ()
{
	SafeDelete ( m_pImpl );
}

void NetPooller_c::SetupEvent ( NetPollEvent_t * pEvent )
{
	assert ( m_pImpl );
	m_pImpl->SetupEvent ( pEvent );
}

void NetPooller_c::Wait ( int timeoutMs )
{
	assert ( m_pImpl );
	m_pImpl->Wait ( timeoutMs );
}

int NetPooller_c::GetNumOfReady () const
{
	assert ( m_pImpl );
	return m_pImpl->GetNumOfReady();
}

void NetPooller_c::ForAll ( std::function<void ( NetPollEvent_t * )> && fnAction )
{
	assert ( m_pImpl );
	m_pImpl->ForAll ( std::move ( fnAction ) );
}

void NetPooller_c::RemoveEvent ( NetPollEvent_t * pEvent )
{
	assert ( m_pImpl );
	m_pImpl->RemoveEvent ( pEvent );
}

void NetPooller_c::DeactivateTimer ( NetPollEvent_t * pEvent )
{
	assert ( m_pImpl );
	m_pImpl->DeactivateTimer ( pEvent );
}

void NetPooller_c::Unlink ( NetPollEvent_t * pEvent, bool bWillClose )
{
	assert ( m_pImpl );
	m_pImpl->Unlink ( pEvent, bWillClose );
}

void NetPooller_c::Shutdown ()
{
	assert ( m_pImpl );
	m_pImpl->Shutdown ();
}