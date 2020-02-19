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

#include "tasksavestate.h"
#include "searchdtask.h"
#include "searchdaemon.h"
#include "sphinxplugin.h"
#include "searchdsql.h"

/////////////////////////////////////////////////////////////////////////////
// User variables stuff: store, add, provide hook
/////////////////////////////////////////////////////////////////////////////

static RwLock_t g_tUservarsMutex;
static SmallStringHash_T<Uservar_t> g_hUservars GUARDED_BY ( g_tUservarsMutex );

static void UservarAdd ( const CSphString& sName, CSphVector<SphAttr_t>& dVal )
{
	ScWL_t wLock ( g_tUservarsMutex );
	Uservar_t* pVar = g_hUservars ( sName );
	if ( pVar )
	{
		// variable exists, release previous value
		// actual destruction of the value (aka data) might happen later
		// as the concurrent queries might still be using and holding that data
		// from here, the old value becomes nameless, though
		assert ( pVar->m_eType==USERVAR_INT_SET );
		assert ( pVar->m_pVal );
	} else
	{
		// create a shiny new variable
		g_hUservars.Add ( Uservar_t(), sName );
		pVar = g_hUservars ( sName );
	}

	// swap in the new value
	assert ( pVar );
	pVar->m_eType = USERVAR_INT_SET;
	pVar->m_pVal = new UservarIntSetValues_c; // previous will be auto-released here
	pVar->m_pVal->SwapData ( dVal );
}

// create or update the variable
void SetLocalUserVar ( const CSphString& sName, CSphVector<SphAttr_t>& dSetValues )
{
	UservarAdd ( sName, dSetValues );
	SphinxqlStateFlush ();
}

UservarIntSet_c UservarsHook ( const CSphString& sUservar )
{
	ScRL_t rLock ( g_tUservarsMutex );
	Uservar_t* pVar = g_hUservars ( sUservar );
	if ( !pVar )
		return UservarIntSet_c ();

	assert ( pVar->m_eType==USERVAR_INT_SET );
	return pVar->m_pVal;
}

/////////////////////////////////////////////////////////////////////////////
// SphinxQL state (plugins, uservars) management
/////////////////////////////////////////////////////////////////////////////
static CSphString g_sSphinxqlState;

/// process a single line from sphinxql state/startup script
static bool SphinxqlStateLine ( CSphVector<char>& dLine, CSphString* sError )
{
	assert ( sError );
	if ( !dLine.GetLength ())
		return true;

	// parser expects CSphString buffer with gap bytes at the end
	if ( dLine.Last ()==';' )
		dLine.Pop ();
	dLine.Add ( '\0' );
	dLine.Add ( '\0' );
	dLine.Add ( '\0' );

	CSphVector <SqlStmt_t> dStmt;
	bool bParsedOK = sphParseSqlQuery ( dLine.Begin (), dLine.GetLength (), dStmt, *sError, SPH_COLLATION_DEFAULT );
	if ( !bParsedOK )
		return false;

	bool bOk = true;
	ARRAY_FOREACH ( i, dStmt )
	{
		SqlStmt_t& tStmt = dStmt[i];
		if ( tStmt.m_eStmt==STMT_SET && tStmt.m_eSet==SET_GLOBAL_UVAR )
		{
			tStmt.m_dSetValues.Sort ();
			UservarAdd ( tStmt.m_sSetName, tStmt.m_dSetValues );
		} else if ( tStmt.m_eStmt==STMT_CREATE_FUNCTION )
		{
			bOk &= sphPluginCreate ( tStmt.m_sUdfLib.cstr (), PLUGIN_FUNCTION, tStmt.m_sUdfName.cstr (),
									 tStmt.m_eUdfType, *sError );

		} else if ( tStmt.m_eStmt==STMT_CREATE_PLUGIN )
		{
			bOk &= sphPluginCreate ( tStmt.m_sUdfLib.cstr (), sphPluginGetType ( tStmt.m_sStringParam ),
									 tStmt.m_sUdfName.cstr (), SPH_ATTR_NONE, *sError );
		} else
		{
			bOk = false;
			sError->SetSprintf ( "unsupported statement (must be one of SET GLOBAL, CREATE FUNCTION, CREATE PLUGIN)" );
		}
	}

	return bOk;
}

/// uservars table reader
static void SphinxqlStateRead ( const CSphString& sName )
{
	if ( sName.IsEmpty ())
		return;

	CSphString sError;
	CSphAutoreader tReader;
	if ( !tReader.Open ( sName, sError ))
		return;

	const int iReadBlock = 32 * 1024;
	const int iGapLen = 2;
	CSphVector<char> dLine;
	dLine.Reserve ( iReadBlock + iGapLen );

	bool bEscaped = false;
	int iLines = 0;
	while ( true )
	{
		const BYTE* pData = NULL;
		int iRead = tReader.GetBytesZerocopy ( &pData, iReadBlock );
		// all uservars got read
		if ( iRead<=0 )
			break;

		// read escaped line
		dLine.Reserve ( dLine.GetLength () + iRead + iGapLen );
		const BYTE* s = pData;
		const BYTE* pEnd = pData + iRead;
		while ( s<pEnd )
		{
			// goto next line for escaped string
			if ( *s=='\\' || ( bEscaped && ( *s=='\n' || *s=='\r' )))
			{
				s++;
				while ( s<pEnd && ( *s=='\n' || *s=='\r' ))
				{
					iLines += ( *s=='\n' );
					s++;
				}
				bEscaped = ( s>=pEnd );
				continue;
			}

			bEscaped = false;
			if ( *s=='\n' || *s=='\r' )
			{
				if ( !SphinxqlStateLine ( dLine, &sError ))
					sphWarning ( "sphinxql_state: parse error at line %d: %s", 1 + iLines, sError.cstr ());

				dLine.Resize ( 0 );
				s++;
				while ( s<pEnd && ( *s=='\n' || *s=='\r' ))
				{
					iLines += ( *s=='\n' );
					s++;
				}
				continue;
			}

			dLine.Add ( *s );
			s++;
		}
	}

	if ( !SphinxqlStateLine ( dLine, &sError ))
		sphWarning ( "sphinxql_state: parse error at line %d: %s", 1 + iLines, sError.cstr ());
}

bool InitSphinxqlState ( CSphString dStateFilePath, CSphString& sError )
{
	g_sSphinxqlState = std::move ( dStateFilePath );
	if ( !g_sSphinxqlState.IsEmpty ())
	{
		SphinxqlStateRead ( g_sSphinxqlState );
		SphinxqlStateFlush ();

		CSphWriter tWriter;
		CSphString sNewState;
		sNewState.SetSprintf ( "%s.new", g_sSphinxqlState.cstr ());
		// initial check that work can be done
		bool bCanWrite = tWriter.OpenFile ( sNewState, sError );
		tWriter.CloseFile ();
		::unlink ( sNewState.cstr ());

		if ( !bCanWrite )
		{
			g_sSphinxqlState = ""; // need to disable thread join on shutdown
			return false;
		}
	}
	return true;
}

struct NamedRefVectorPair_t
{
	CSphString m_sName;
	UservarIntSet_c m_pVal;
};

/// SphinxQL state writer
/// periodically flushes changes of uservars, UDFs
static void SphinxqlStateThreadFunc ( void* )
{
	assert ( !g_sSphinxqlState.IsEmpty ());
	CSphString sNewState;
	sNewState.SetSprintf ( "%s.new", g_sSphinxqlState.cstr ());

	char dBuf[512];
	const int iMaxString = 80;
	assert (( int ) sizeof ( dBuf )>iMaxString );

	CSphString sError;
	CSphWriter tWriter;

	// stand still till save time
	// close and truncate the .new file
	if ( !tWriter.OpenFile ( sNewState, sError ))
	{
		sphWarning ( "sphinxql_state flush failed: %s", sError.cstr ());
		return;
	}

	/////////////
	// save UDFs
	/////////////

	ThreadSystem_t tThdSystemDesc ( "SphinxQL state save" );

	sphPluginSaveState ( tWriter ); // refactor!

	/////////////////
	// save uservars
	/////////////////

	CSphVector <NamedRefVectorPair_t> dUservars;
	{
		ScRL_t rLock ( g_tUservarsMutex );
		dUservars.Reserve ( g_hUservars.GetLength ());
		g_hUservars.IterateStart ();
		while ( g_hUservars.IterateNext ())
		{
			if ( !g_hUservars.IterateGet ().m_pVal->GetLength ())
				continue;

			auto& tPair = dUservars.Add ();
			tPair.m_sName = g_hUservars.IterateGetKey ();
			tPair.m_pVal = g_hUservars.IterateGet ().m_pVal;
		}
	}
	dUservars.Sort ( bind ( &NamedRefVectorPair_t::m_sName ));

	// reinitiate store process on new variables added
	for ( const auto& dUserVar : dUservars )
	{
		const CSphVector <SphAttr_t>& dVals = *dUserVar.m_pVal;
		int iLen = snprintf ( dBuf, sizeof ( dBuf ), "SET GLOBAL %s = ( "
		INT64_FMT, dUserVar.m_sName.cstr (), dVals[0] );
		for ( int j = 1; j<dVals.GetLength (); j++ )
		{
			iLen += snprintf ( dBuf + iLen, sizeof ( dBuf ), ", "
			INT64_FMT, dVals[j] );

			if ( iLen>=iMaxString && j<dVals.GetLength () - 1 )
			{
				iLen += snprintf ( dBuf + iLen, sizeof ( dBuf ), " \\\n" );
				tWriter.PutBytes ( dBuf, iLen );
				iLen = 0;
			}
		}

		if ( iLen )
			tWriter.PutBytes ( dBuf, iLen );

		char sTail[] = " );\n";
		tWriter.PutBytes ( sTail, sizeof ( sTail ) - 1 );
	}

	/////////////////////////////////
	// writing done, flip the burger
	/////////////////////////////////

	tWriter.CloseFile ();
	if ( sph::rename ( sNewState.cstr (), g_sSphinxqlState.cstr ())==0 )
	{
		::unlink ( sNewState.cstr ());
	} else
	{
		sphWarning ( "sphinxql_state flush: rename %s to %s failed: %s",
					 sNewState.cstr (), g_sSphinxqlState.cstr (), strerrorm ( errno ));
	}
}

void SphinxqlStateFlush ()
{
	if ( g_sSphinxqlState.IsEmpty ())
		return;

	static int iSaveSphinxql = -1;
	if ( iSaveSphinxql<0 )
		iSaveSphinxql = TaskManager::RegisterGlobal ( "SphinxQL state flush", SphinxqlStateThreadFunc, nullptr, 1, 1 );
	TaskManager::StartJob ( iSaveSphinxql );
}
