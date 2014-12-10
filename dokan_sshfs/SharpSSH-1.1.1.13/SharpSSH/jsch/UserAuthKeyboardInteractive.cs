using System;

namespace Tamir.SharpSsh.jsch
{
/* -*-mode:java; c-basic-offset:2; -*- */
/*
Copyright (c) 2002,2003,2004 ymnk, JCraft,Inc. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright 
     notice, this list of conditions and the following disclaimer in 
     the documentation and/or other materials provided with the distribution.

  3. The names of the authors may not be used to endorse or promote products
     derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL JCRAFT,
INC. OR ANY CONTRIBUTORS TO THIS SOFTWARE BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

class UserAuthKeyboardInteractive : UserAuth{
  internal UserInfo userinfo;
  internal UserAuthKeyboardInteractive(UserInfo userinfo){
   this.userinfo=userinfo;
  }

  public override bool start(Session session) {
//System.out.println("UserAuthKeyboardInteractive: start");
    Packet packet=session.packet;
    Buffer buf=session.buf;
    String username=session.username;
    String dest=username+"@"+session.host;
    if(session.port!=22){
      dest+=(":"+session.port);
    }

    bool cancel=false;

    byte[] _username=null;
    try{ _username=System.Text.Encoding.UTF8.GetBytes(username); }
    catch{
      _username=Util.getBytes(username);
    }

    while(true){
      // send
      // byte      SSH_MSG_USERAUTH_REQUEST(50)
      // string    user name (ISO-10646 UTF-8, as defined in [RFC-2279])
      // string    service name (US-ASCII) "ssh-userauth" ? "ssh-connection"
      // string    "keyboard-interactive" (US-ASCII)
      // string    language tag (as defined in [RFC-3066])
      // string    submethods (ISO-10646 UTF-8)
      packet.reset();
      buf.putByte((byte)Session.SSH_MSG_USERAUTH_REQUEST);
      buf.putString(_username);
      buf.putString(Util.getBytes("ssh-connection"));
      //buf.putString("ssh-userauth".getBytes());
      buf.putString(Util.getBytes("keyboard-interactive"));
      buf.putString(Util.getBytes(""));
      buf.putString(Util.getBytes(""));
      session.write(packet);

      bool firsttime=true;
      loop:
      while(true){
	// receive
	// byte      SSH_MSG_USERAUTH_SUCCESS(52)
	// string    service name
	try{  buf=session.read(buf); }
	catch(JSchException e){
		e.GetType();
	  return false;
	}
	catch(System.IO.IOException e){
		e.GetType();
	  return false;
	}
	//System.out.println("read: 52 ? "+    buf.buffer[5]);
	if(buf.buffer[5]==Session.SSH_MSG_USERAUTH_SUCCESS){
	  return true;
	}
	if(buf.buffer[5]==Session.SSH_MSG_USERAUTH_BANNER){
	  buf.getInt(); buf.getByte(); buf.getByte();
	  byte[] _message=buf.getString();
	  byte[] lang=buf.getString();
	  String message=null;
	  try{ message=Util.getStringUTF8(_message); }
	  catch{
	    message=Util.getString(_message);
	  }
	  if(userinfo!=null){
	    userinfo.showMessage(message);
	  }
	  goto loop;
	}
	if(buf.buffer[5]==Session.SSH_MSG_USERAUTH_FAILURE){
	  buf.getInt(); buf.getByte(); buf.getByte(); 
	  byte[] foo=buf.getString();
	  int partial_success=buf.getByte();
//	  System.out.println(new String(foo)+
//			     " partial_success:"+(partial_success!=0));

	  if(partial_success!=0){
	    throw new JSchPartialAuthException(Util.getString(foo));
	  }

	  if(firsttime){
	    throw new JSchException("USERAUTH KI is not supported");
	    //return false;
	    //cancel=true;  // ??
	  }
	  break;
	}
	if(buf.buffer[5]==Session.SSH_MSG_USERAUTH_INFO_REQUEST){
	  firsttime=false;
	  buf.getInt(); buf.getByte(); buf.getByte();
	  String name=Util.getString(buf.getString());
	  String instruction=Util.getString(buf.getString());
	  String languate_tag=Util.getString(buf.getString());
	  int num=buf.getInt();
//System.out.println("name: "+name);
//System.out.println("instruction: "+instruction);
//System.out.println("lang: "+languate_tag);
//System.out.println("num: "+num);
	  String[] prompt=new String[num];
	  bool[] echo=new bool[num];
	  for(int i=0; i<num; i++){
	    prompt[i]=Util.getString(buf.getString());
	    echo[i]=(buf.getByte()!=0);
//System.out.println("  "+prompt[i]+","+echo[i]);
	  }

	  String[] response=null;
	  if(num>0
	     ||(name.Length>0 || instruction.Length>0)
	     ){
	    UIKeyboardInteractive kbi=(UIKeyboardInteractive)userinfo;
	    if(userinfo!=null){
	    response=kbi.promptKeyboardInteractive(dest,
						   name,
						   instruction,
						   prompt,
						   echo);
	    }
	  }
	  // byte      SSH_MSG_USERAUTH_INFO_RESPONSE(61)
	  // int       num-responses
	  // string    response[1] (ISO-10646 UTF-8)
	  // ...
	  // string    response[num-responses] (ISO-10646 UTF-8)
//if(response!=null)
//System.out.println("response.length="+response.length);
//else
//System.out.println("response is null");
	  packet.reset();
	  buf.putByte((byte)Session.SSH_MSG_USERAUTH_INFO_RESPONSE);
	  if(num>0 &&
	     (response==null ||  // cancel
	      num!=response.Length)){
	    buf.putInt(0);
	    if(response==null)
	      cancel=true;
	  }
	  else{
	    buf.putInt(num);
	    for(int i=0; i<num; i++){
//System.out.println("response: |"+response[i]+"| <- replace here with **** if you need");
	      buf.putString(Util.getBytes(response[i]));
	    }
	  }
	  session.write(packet);
	  if(cancel)
	    break;
//System.out.println("continue loop");
	  goto loop;
	}
	//throw new JSchException("USERAUTH fail ("+buf.buffer[5]+")");
	return false;
      }
      if(cancel){
	throw new JSchAuthCancelException("keyboard-interactive");
	//break;
      }
    }
    //return false;
  }
}

}
