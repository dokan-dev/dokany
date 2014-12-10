using System;
using System.Net;
using System.IO;
using Tamir.Streams;
using System.Runtime.CompilerServices;
using Tamir.SharpSsh.java.lang;
using Str = Tamir.SharpSsh.java.String;

namespace Tamir.SharpSsh.jsch
{
	/* -*-mode:java; c-basic-offset:2; -*- */
	/*
	Copyright (c) 2002,2003,2004 ymnk, JCraft,Inc. All rights reserved.

	Redistribution and use In source and binary forms, with or without
	modification, are permitted provided that the following conditions are met:

	  1. Redistributions of source code must retain the above copyright notice,
		 this list of conditions and the following disclaimer.

	  2. Redistributions In binary form must reproduce the above copyright 
		 notice, this list of conditions and the following disclaimer In 
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


	public abstract class Channel : Tamir.SharpSsh.java.lang.Runnable
	{
		internal static int index=0; 
		private static java.util.Vector pool=new java.util.Vector();
		internal static Channel getChannel(String type)
		{
			if(type.Equals("session"))
			{
				return new ChannelSession();
			}
			if(type.Equals("shell"))
			{
				return new ChannelShell();
			}
			if(type.Equals("exec"))
			{
				return new ChannelExec();
			}
			if(type.Equals("x11"))
			{
				return new ChannelX11();
			}
			if(type.Equals("direct-tcpip"))
			{
				return new ChannelDirectTCPIP();
			}
			if(type.Equals("forwarded-tcpip"))
			{
				return new ChannelForwardedTCPIP();
			}
			if(type.Equals("sftp"))
			{
				return new ChannelSftp();
			}
			if(type.Equals("subsystem"))
			{
				return new ChannelSubsystem();
			}
			return null;
		}
		internal static Channel getChannel(int id, Session session)
		{
			lock(pool)
			{
				for(int i=0; i<pool.size(); i++)
				{
					Channel c=(Channel)(pool.elementAt(i));
					if(c.id==id && c.session==session) return c;
				}
			}
			return null;
		}
		internal static void del(Channel c)
		{
			lock(pool)
			{
				pool.removeElement(c);
			}
		}

		internal int id;
		internal int recipient=-1;
		internal byte[] type=new Str("foo").getBytes();
		internal int lwsize_max=0x100000;
		internal int lwsize=0x100000;  // local initial window size
		internal int lmpsize=0x4000;     // local maximum packet size

		internal int rwsize=0;         // remote initial window size
		internal int rmpsize=0;        // remote maximum packet size

		internal IO io=null;    
		internal Thread thread=null;

		internal bool eof_local=false;
		internal bool _eof_remote=false;

		internal bool _close=false;
		internal bool connected=false;

		internal int exitstatus=-1;

		internal int reply=0; 

		internal Session session;

		internal Channel()
		{
			lock(pool)
			{
				id=index++;
				pool.addElement(this);
			}
		}
		internal virtual void setRecipient(int foo)
		{
			this.recipient=foo;
		}
		internal virtual int getRecipient()
		{
			return recipient;
		}

		public virtual void init()
		{
		}

		public virtual void connect()
		{
			if(!session.isConnected())
			{
				throw new JSchException("session is down");
			}
			try
			{
				Buffer buf=new Buffer(100);
				Packet packet=new Packet(buf);
				// send
				// byte   SSH_MSG_CHANNEL_OPEN(90)
				// string channel type         //
				// uint32 sender channel       // 0
				// uint32 initial window size  // 0x100000(65536)
				// uint32 maxmum packet size   // 0x4000(16384)
				packet.reset();
				buf.putByte((byte)90);
				buf.putString(this.type);
				buf.putInt(this.id);
				buf.putInt(this.lwsize);
				buf.putInt(this.lmpsize);
				session.write(packet);

				int retry=1000;
				while(this.getRecipient()==-1 &&
					session.isConnected() &&
					retry>0)
				{
					try{Thread.sleep(50);}
					catch(Exception ee){}
					retry--;
				}
				if(!session.isConnected())
				{
					throw new JSchException("session is down");
				}
				if(retry==0)
				{
					throw new JSchException("channel is not opened.");
				}
				connected=true;
				start();
			}
			catch(Exception e)
			{
				connected=false;
				if(e is JSchException) throw (JSchException)e;
			}
		}

		public virtual void setXForwarding(bool foo)
		{
		}

		public virtual void start(){}

		public bool isEOF() {return _eof_remote;}

		internal virtual void getData(Buffer buf)
		{
			setRecipient(buf.getInt());
			setRemoteWindowSize(buf.getInt());
			setRemotePacketSize(buf.getInt());
		}

		public virtual void setInputStream(Stream In)
		{
			io.setInputStream(In, false);
		}
		public virtual void setInputStream(Stream In, bool dontclose)
		{
			io.setInputStream(In, dontclose);
		}
		public virtual void setOutputStream(Stream Out)
		{
			io.setOutputStream(Out, false);
		}
		public virtual void setOutputStream(Stream Out, bool dontclose)
		{
			io.setOutputStream(Out, dontclose);
		}
		public virtual void setExtOutputStream(Stream Out)
		{
			io.setExtOutputStream(Out, false);
		}
		public virtual void setExtOutputStream(Stream Out, bool dontclose)
		{
			io.setExtOutputStream(Out, dontclose);
		}
		public virtual java.io.InputStream getInputStream()  
		{
			PipedInputStream In=
				new MyPipedInputStream(
				32*1024  // this value should be customizable.
				);
			io.setOutputStream(new PassiveOutputStream(In), false);
			return In;
		}
		public virtual java.io.InputStream getExtInputStream()  
		{
			PipedInputStream In=
				new MyPipedInputStream(
				32*1024  // this value should be customizable.
				);
			io.setExtOutputStream(new PassiveOutputStream(In), false);
			return In;
		}
		public virtual Stream getOutputStream()  
		{
			PipedOutputStream Out=new PipedOutputStream();
			io.setInputStream(new PassiveInputStream(Out
				, 32*1024
				), false);
			//  io.setInputStream(new PassiveInputStream(Out), false);
			return Out;
		}
		internal class MyPipedInputStream : PipedInputStream
		{
			internal MyPipedInputStream():base() { ; }
			internal MyPipedInputStream(int size) :base()
			{
				buffer=new byte[size];
			}
			internal MyPipedInputStream(PipedOutputStream Out):base(Out) { }
			internal MyPipedInputStream(PipedOutputStream Out, int size):base(Out) 
			{
				buffer=new byte[size];
			}
		}
		internal virtual void setLocalWindowSizeMax(int foo){ this.lwsize_max=foo; }
		internal virtual void setLocalWindowSize(int foo){ this.lwsize=foo; }
		internal virtual void setLocalPacketSize(int foo){ this.lmpsize=foo; }
		[System.Runtime.CompilerServices.MethodImpl(MethodImplOptions.Synchronized)]
		internal virtual void setRemoteWindowSize(int foo){ this.rwsize=foo; }
		[System.Runtime.CompilerServices.MethodImpl(MethodImplOptions.Synchronized)]
		internal virtual void addRemoteWindowSize(int foo){ this.rwsize+=foo; }
		internal virtual void setRemotePacketSize(int foo){ this.rmpsize=foo; }

		public virtual void run()
		{
		}

		internal virtual void write(byte[] foo)  
		{
			write(foo, 0, foo.Length);
		}
		internal virtual void write(byte[] foo, int s, int l)  
		{
			try
			{
				//    if(io.outs!=null)
				io.put(foo, s, l);
			}
			catch(NullReferenceException e){}
		}
		internal virtual void write_ext(byte[] foo, int s, int l)  
		{
			try
			{
				//    if(io.out_ext!=null)
				io.put_ext(foo, s, l);
			}
			catch(NullReferenceException e){}
		}

		internal virtual void eof_remote()
		{
			_eof_remote=true;
			try
			{
				if(io.outs!=null)
				{
					io.outs.Close();
					io.outs=null;
				}
			}
			catch(NullReferenceException e){}
			catch(IOException e){}
		}

		internal virtual void eof()
		{
			//System.Out.println("EOF!!!! "+this);
			//Thread.dumpStack();
			if(_close)return;
			if(eof_local)return;
			eof_local=true;
			//close=eof;
			try
			{
				Buffer buf=new Buffer(100);
				Packet packet=new Packet(buf);
				packet.reset();
				buf.putByte((byte)Session.SSH_MSG_CHANNEL_EOF);
				buf.putInt(getRecipient());
				session.write(packet);
			}
			catch(Exception e)
			{
				//System.Out.println("Channel.eof");
				//e.printStackTrace();
			}
			/*
			if(!isConnected()){ disconnect(); }
			*/
		}

		/*
		http://www1.ietf.org/internet-drafts/draft-ietf-secsh-connect-24.txt

	  5.3  Closing a Channel
		When a party will no longer send more data to a channel, it SHOULD
		 send SSH_MSG_CHANNEL_EOF.

				  byte      SSH_MSG_CHANNEL_EOF
				  uint32    recipient_channel

		No explicit response is sent to this message.  However, the
		 application may send EOF to whatever is at the other end of the
		channel.  Note that the channel remains open after this message, and
		 more data may still be sent In the other direction.  This message
		 does not consume window space and can be sent even if no window space
		 is available.

		   When either party wishes to terminate the channel, it sends
		   SSH_MSG_CHANNEL_CLOSE.  Upon receiving this message, a party MUST
		 send back a SSH_MSG_CHANNEL_CLOSE unless it has already sent this
		 message for the channel.  The channel is considered closed for a
		   party when it has both sent and received SSH_MSG_CHANNEL_CLOSE, and
		 the party may then reuse the channel number.  A party MAY send
		 SSH_MSG_CHANNEL_CLOSE without having sent or received
		 SSH_MSG_CHANNEL_EOF.

				  byte      SSH_MSG_CHANNEL_CLOSE
				  uint32    recipient_channel

		 This message does not consume window space and can be sent even if no
		 window space is available.

		 It is recommended that any data sent before this message is delivered
		   to the actual destination, if possible.
		*/

		internal virtual void close()
		{
			//System.Out.println("close!!!!");
			if(_close)return;
			_close=true;
			try
			{
				Buffer buf=new Buffer(100);
				Packet packet=new Packet(buf);
				packet.reset();
				buf.putByte((byte)Session.SSH_MSG_CHANNEL_CLOSE);
				buf.putInt(getRecipient());
				session.write(packet);
			}
			catch(Exception e)
			{
				//e.printStackTrace();
			}
		}
		public virtual bool isClosed()
		{
			return _close;
		}
		internal static void disconnect(Session session)
		{
			Channel[] channels=null;
			int count=0;
			lock(pool)
			{
				channels=new Channel[pool.size()];
				for(int i=0; i<pool.size(); i++)
				{
					try
					{
						Channel c=((Channel)(pool.elementAt(i)));
						if(c.session==session)
						{
							channels[count++]=c;
						}
					}
					catch(Exception e)
					{
					}
				} 
			}
			for(int i=0; i<count; i++)
			{
				channels[i].disconnect();
			}
		}

		/*
		public void finalize() throws Throwable{
		  disconnect();
		  super.finalize();
		  session=null;
		}
		*/

		public virtual void disconnect()
		{
			//System.Out.println(this+":disconnect "+io+" "+io.in);
			if(!connected)
			{
				return;
			}
			connected=false;

			close();

			_eof_remote=eof_local=true;

			thread=null;

			try
			{
				if(io!=null)
				{
					io.close();
				}
			}
			catch(Exception e)
			{
				//e.printStackTrace();
			}
			io=null;
			Channel.del(this);
		}

		public virtual bool isConnected()
		{
			if(this.session!=null)
			{
				return session.isConnected() && connected;
			}
			return false;
		}

		public virtual void sendSignal(String foo)  
		{
			RequestSignal request=new RequestSignal();
			request.setSignal(foo);
			request.request(session, this);
		}

		//  public String toString(){
		//      return "Channel: type="+new String(type)+",id="+id+",recipient="+recipient+",window_size="+window_size+",packet_size="+packet_size;
		//  }

		/*
		  class OutputThread extends Thread{
			Channel c;
			OutputThread(Channel c){ this.c=c;}
			public void run(){c.output_thread();}
		  }
		*/

		internal class PassiveInputStream : MyPipedInputStream
		{
			internal PipedOutputStream Out;
			internal PassiveInputStream(PipedOutputStream Out, int size) :base(Out, size)
			{
				this.Out=Out;
			}
			internal PassiveInputStream(PipedOutputStream Out):base(Out) 
			{
				this.Out=Out;
			}
			public override void close() 
			{
				if(Out!=null)
				{
					this.Out.close();
				}
				Out=null;
			}
		}
		internal class PassiveOutputStream : PipedOutputStream
		{
			internal PassiveOutputStream(PipedInputStream In) :base(In)
			{
			}
		}

		internal virtual void setExitStatus(int foo){ exitstatus=foo; }
		public virtual int getExitStatus(){ return exitstatus; }

		internal virtual void setSession(Session session)
		{
			this.session=session;
		}
		public virtual Session getSession(){ return session; }
		public virtual int getId(){ return id; }
		//public int getRecipientId(){ return getRecipient(); }

	}
}
