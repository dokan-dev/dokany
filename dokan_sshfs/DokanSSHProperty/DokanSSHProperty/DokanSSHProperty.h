

/* this ALWAYS GENERATED file contains the definitions for the interfaces */


 /* File created by MIDL compiler version 6.00.0366 */
/* at Tue Jun 10 22:15:10 2008
 */
/* Compiler settings for .\DokanSSHProperty.idl:
    Oicf, W1, Zp8, env=Win32 (32b run)
    protocol : dce , ms_ext, c_ext
    error checks: allocation ref bounds_check enum stub_data 
    VC __declspec() decoration level: 
         __declspec(uuid()), __declspec(selectany), __declspec(novtable)
         DECLSPEC_UUID(), MIDL_INTERFACE()
*/
//@@MIDL_FILE_HEADING(  )

#pragma warning( disable: 4049 )  /* more than 64k source lines */


/* verify that the <rpcndr.h> version is high enough to compile this file*/
#ifndef __REQUIRED_RPCNDR_H_VERSION__
#define __REQUIRED_RPCNDR_H_VERSION__ 440
#endif

#include "rpc.h"
#include "rpcndr.h"

#ifndef __RPCNDR_H_VERSION__
#error this stub requires an updated version of <rpcndr.h>
#endif // __RPCNDR_H_VERSION__

#ifndef COM_NO_WINDOWS_H
#include "windows.h"
#include "ole2.h"
#endif /*COM_NO_WINDOWS_H*/

#ifndef __DokanSSHProperty_h__
#define __DokanSSHProperty_h__

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
#pragma once
#endif

/* Forward Declarations */ 

#ifndef __ISSHProperty_FWD_DEFINED__
#define __ISSHProperty_FWD_DEFINED__
typedef interface ISSHProperty ISSHProperty;
#endif 	/* __ISSHProperty_FWD_DEFINED__ */


#ifndef __SSHProperty_FWD_DEFINED__
#define __SSHProperty_FWD_DEFINED__

#ifdef __cplusplus
typedef class SSHProperty SSHProperty;
#else
typedef struct SSHProperty SSHProperty;
#endif /* __cplusplus */

#endif 	/* __SSHProperty_FWD_DEFINED__ */


/* header files for imported files */
#include "oaidl.h"
#include "ocidl.h"

#ifdef __cplusplus
extern "C"{
#endif 

void * __RPC_USER MIDL_user_allocate(size_t);
void __RPC_USER MIDL_user_free( void * ); 

#ifndef __ISSHProperty_INTERFACE_DEFINED__
#define __ISSHProperty_INTERFACE_DEFINED__

/* interface ISSHProperty */
/* [unique][helpstring][uuid][object] */ 


EXTERN_C const IID IID_ISSHProperty;

#if defined(__cplusplus) && !defined(CINTERFACE)
    
    MIDL_INTERFACE("A2D061A2-A6F7-4ECD-953C-84C1623DFBCE")
    ISSHProperty : public IUnknown
    {
    public:
    };
    
#else 	/* C style interface */

    typedef struct ISSHPropertyVtbl
    {
        BEGIN_INTERFACE
        
        HRESULT ( STDMETHODCALLTYPE *QueryInterface )( 
            ISSHProperty * This,
            /* [in] */ REFIID riid,
            /* [iid_is][out] */ void **ppvObject);
        
        ULONG ( STDMETHODCALLTYPE *AddRef )( 
            ISSHProperty * This);
        
        ULONG ( STDMETHODCALLTYPE *Release )( 
            ISSHProperty * This);
        
        END_INTERFACE
    } ISSHPropertyVtbl;

    interface ISSHProperty
    {
        CONST_VTBL struct ISSHPropertyVtbl *lpVtbl;
    };

    

#ifdef COBJMACROS


#define ISSHProperty_QueryInterface(This,riid,ppvObject)	\
    (This)->lpVtbl -> QueryInterface(This,riid,ppvObject)

#define ISSHProperty_AddRef(This)	\
    (This)->lpVtbl -> AddRef(This)

#define ISSHProperty_Release(This)	\
    (This)->lpVtbl -> Release(This)


#endif /* COBJMACROS */


#endif 	/* C style interface */




#endif 	/* __ISSHProperty_INTERFACE_DEFINED__ */



#ifndef __DokanSSHPropertyLib_LIBRARY_DEFINED__
#define __DokanSSHPropertyLib_LIBRARY_DEFINED__

/* library DokanSSHPropertyLib */
/* [helpstring][version][uuid] */ 


EXTERN_C const IID LIBID_DokanSSHPropertyLib;

EXTERN_C const CLSID CLSID_SSHProperty;

#ifdef __cplusplus

class DECLSPEC_UUID("50A9DC6D-948D-4219-B378-6E0B28019FF7")
SSHProperty;
#endif
#endif /* __DokanSSHPropertyLib_LIBRARY_DEFINED__ */

/* Additional Prototypes for ALL interfaces */

/* end of Additional Prototypes */

#ifdef __cplusplus
}
#endif

#endif


