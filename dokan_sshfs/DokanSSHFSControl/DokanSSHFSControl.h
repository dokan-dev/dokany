

/* this ALWAYS GENERATED file contains the definitions for the interfaces */


 /* File created by MIDL compiler version 6.00.0366 */
/* at Tue Jun 10 22:14:22 2008
 */
/* Compiler settings for .\DokanSSHFSControl.idl:
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

#ifndef __DokanSSHFSControl_h__
#define __DokanSSHFSControl_h__

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
#pragma once
#endif

/* Forward Declarations */ 

#ifndef __ISSHFSControl_FWD_DEFINED__
#define __ISSHFSControl_FWD_DEFINED__
typedef interface ISSHFSControl ISSHFSControl;
#endif 	/* __ISSHFSControl_FWD_DEFINED__ */


#ifndef __SSHFSControl_FWD_DEFINED__
#define __SSHFSControl_FWD_DEFINED__

#ifdef __cplusplus
typedef class SSHFSControl SSHFSControl;
#else
typedef struct SSHFSControl SSHFSControl;
#endif /* __cplusplus */

#endif 	/* __SSHFSControl_FWD_DEFINED__ */


/* header files for imported files */
#include "oaidl.h"
#include "ocidl.h"

#ifdef __cplusplus
extern "C"{
#endif 

void * __RPC_USER MIDL_user_allocate(size_t);
void __RPC_USER MIDL_user_free( void * ); 

#ifndef __ISSHFSControl_INTERFACE_DEFINED__
#define __ISSHFSControl_INTERFACE_DEFINED__

/* interface ISSHFSControl */
/* [unique][helpstring][uuid][object] */ 


EXTERN_C const IID IID_ISSHFSControl;

#if defined(__cplusplus) && !defined(CINTERFACE)
    
    MIDL_INTERFACE("168172AB-0E52-4789-A276-1AA3BDE72064")
    ISSHFSControl : public IUnknown
    {
    public:
    };
    
#else 	/* C style interface */

    typedef struct ISSHFSControlVtbl
    {
        BEGIN_INTERFACE
        
        HRESULT ( STDMETHODCALLTYPE *QueryInterface )( 
            ISSHFSControl * This,
            /* [in] */ REFIID riid,
            /* [iid_is][out] */ void **ppvObject);
        
        ULONG ( STDMETHODCALLTYPE *AddRef )( 
            ISSHFSControl * This);
        
        ULONG ( STDMETHODCALLTYPE *Release )( 
            ISSHFSControl * This);
        
        END_INTERFACE
    } ISSHFSControlVtbl;

    interface ISSHFSControl
    {
        CONST_VTBL struct ISSHFSControlVtbl *lpVtbl;
    };

    

#ifdef COBJMACROS


#define ISSHFSControl_QueryInterface(This,riid,ppvObject)	\
    (This)->lpVtbl -> QueryInterface(This,riid,ppvObject)

#define ISSHFSControl_AddRef(This)	\
    (This)->lpVtbl -> AddRef(This)

#define ISSHFSControl_Release(This)	\
    (This)->lpVtbl -> Release(This)


#endif /* COBJMACROS */


#endif 	/* C style interface */




#endif 	/* __ISSHFSControl_INTERFACE_DEFINED__ */



#ifndef __DokanSSHFSControlLib_LIBRARY_DEFINED__
#define __DokanSSHFSControlLib_LIBRARY_DEFINED__

/* library DokanSSHFSControlLib */
/* [helpstring][version][uuid] */ 


EXTERN_C const IID LIBID_DokanSSHFSControlLib;

EXTERN_C const CLSID CLSID_SSHFSControl;

#ifdef __cplusplus

class DECLSPEC_UUID("B4F169EE-929C-4693-B97D-0DAE20CAFD6C")
SSHFSControl;
#endif
#endif /* __DokanSSHFSControlLib_LIBRARY_DEFINED__ */

/* Additional Prototypes for ALL interfaces */

/* end of Additional Prototypes */

#ifdef __cplusplus
}
#endif

#endif


