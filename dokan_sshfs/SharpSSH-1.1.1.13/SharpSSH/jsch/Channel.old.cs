//using System;
//using System.Net;
//using System.IO;
//using Tamir.Streams;
//
//namespace Tamir.SharpSsh.jsch
//{
//	/* -*-mode:java; c-basic-offset:2; -*- */
//	/*
//	Copyright (c) 2002,2003,2004 ymnk, JCraft,Inc. All rights reserved.
//
//	Redistribution and use in source and binary forms, with or without
//	modification, are permitted provided that the following conditions are met:
//
//	  1. Redistributions of source code must retain the above copyright notice,
//		 this list of conditions and the following disclaimer.
//
//	  2. Redistributions in binary form must reproduce the above copyright 
//		 notice, this list of conditions and the following disclaimer in 
//		 the documentation and/or other materials provided with the distribution.
//
//	  3. The names of the authors may not be used to endorse or promote products
//		 derived from this software without specific prior written permission.
//
//	THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED WARRANTIES,
//	INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
//	FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL JCRAFT,
//	INC. OR ANY CONTRIBUTORS TO THIS SOFTWARE BE LIABLE FOR ANY DIRECT, INDIRECT,
//	INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
//	LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
//	OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
//	LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
//	NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
//	EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//	*/
//
//
//	public abstract class Channel
//	{
//		internal static int index=0; 
//		private static System.Collections.ArrayList pool=new System.Collections.ArrayList();
//		internal static Channel getChannel(String type)
//		{
//			if(type.Equals("session"))
//			{
//				return new ChannelSession();
//			}
//			if(type.Equals("shell"))
//			{
//				return new ChannelShell();
//			}
//			if(type.Equals("exec"))
//			{
//				return new ChannelExec();
//			}
//			if(type.Equals("x11"))
//			{
//				return new ChannelX11();
//			}
//			if(type.Equals("direct-tcpip"))
//			{
//				return new ChannelDirectTCPIP();
//			}
//			if(type.Equals("forwarded-tcpip"))
//			{
//				return new ChannelForwardedTCPIP();
//			}
//			if(type.Equals("sftp"))
//			{
//				return new ChannelSftp();
//			}
//			return null;
//		}
//		internal static Channel getChannel(int id, Session session)
//		{
//			lock(pool)
//			{
//				for(int i=0; i<pool.Count; i++)
//				{
//					Channel c=(Channel)(pool[i]);
//					if(c.id==id && c.session==session) return c;
//				}
//			}
//			return null;
//		}
//		internal static void del(Channel c)
//		{
//			lock(pool)
//			{
//				pool.Remove(c);
//			}
//		}
//
//		internal int id;
//		internal int recipient=-1;
//		internal byte[] type= Util.getBytes( "foo" );
//		internal int lwsize_max=0x100000;
//		internal int lwsize=0x100000;  // local initial window size
//		internal  int lmpsize=0x4000;     // local maximum packet size
//
//		internal int rwsize=0;         // remote initial window size
//		internal int rmpsize=0;        // remote maximum packet size
//
//		internal IO io=null;    
//		internal System.Threading.Thread thread=null;
//
//		internal bool eof_local=false;
//		internal bool eof_remote=false;
//
//		internal bool _close=false;
//
//		internal int exitstatus=-1;
//
//		internal int reply=0; 
//
//		internal Session session;
//
//		internal Channel()
//		{
//			lock(pool)
//			{
//				id=index++;
//				pool.Add(this);
//			}
//		}
//		internal void setRecipient(int foo)
//		{
//			this.recipient=foo;
//		}
//		internal virtual int getRecipient()
//		{
//			return recipient;
//		}
//
//		public virtual void init()
//		{
//		}
//
//		public virtual void connect() 
//		{
//			if(!isConnected())
//			{
//				throw new JSchException("session is down");
//			}
//			try
//			{
//				Buffer buf=new Buffer(100);
//				Packet packet=new Packet(buf);
//				// send
//				// byte   SSH_MSG_CHANNEL_OPEN(90)
//				// string channel type         //
//				// uint32 sender channel       // 0
//				// uint32 initial window size  // 0x100000(65536)
//				// uint32 maxmum packet size   // 0x4000(16384)
//				packet.reset();
//				buf.putByte((byte)90);
//				buf.putString(this.type);
//				buf.putInt(this.id);
//				buf.putInt(this.lwsize);
//				buf.putInt(this.lmpsize);
//				session.write(packet);
//
//				int retry=1000;
//				while(this.getRecipient()==-1 &&
//					session.IsConnected() &&
//					retry>0)
//				{
//					try{System.Threading.Thread.Sleep(50);}
//					catch{}
//					retry--;
//				}
//				if(!session.IsConnected())
//				{
//					throw new JSchException("session is down");
//				}
//				if(retry==0)
//				{
//					throw new JSchException("channel is not opened.");
//				}
//				start();
//			}
//			catch(Exception e)
//			{
//				if(e is JSchException) throw (JSchException)e;
//			}
//		}
//
//		public virtual void setXForwarding(bool foo)
//		{
//		}
//
//		public virtual void start() {}
//
//		public bool isEOF() {return eof_remote;}
//
//		internal virtual void getData(Buffer buf)
//		{
//			setRecipient(buf.getInt());
//			setRemoteWindowSize(buf.getInt());
//			setRemotePacketSize(buf.getInt());
//		}
//  
//		public RequestWindowChange getRequestWindowChange() 
//		{
//			return new RequestWindowChange(); 
//		}
//
//		public virtual void setInputStream(Stream ins)
//		{
//			io.setInputStream(ins);
//		}
//		public virtual void setOutputStream(Stream outs)
//		{
//			io.setOutputStream(outs);
//		}
//		public virtual void setExtOutputStream(Stream outs)
//		{
//			io.setExtOutputStream(outs);
//		}
//		public virtual Stream getInputStream() 
//		{
//			PipedInputStream ins=
//				new MyPipedInputStream(
//				32*1024  // this value should be customizable.
//				);
//			io.setOutputStream(new PassiveOutputStream(ins));
//			return ins;
//		}
//		public virtual Stream getExtInputStream() 
//		{
//			PipedInputStream ins=
//				new MyPipedInputStream(
//				32*1024  // this value should be customizable.
//				);
//			io.setExtOutputStream(new PassiveOutputStream(ins));
//			return ins;
//		}
//		public virtual Stream getOutputStream()  
//		{
//			PipedOutputStream outs=new PipedOutputStream();
//			io.setInputStream(new PassiveInputStream(outs));
//			return outs;
//		}
//
//		internal void setLocalWindowSizeMax(int foo){ this.lwsize_max=foo; }
//		internal void setLocalWindowSize(int foo){ this.lwsize=foo; }
//		internal void setLocalPacketSize(int foo){ this.lmpsize=foo; }
//		internal void setRemoteWindowSize(int foo){ this.rwsize=foo; }
//		internal void addRemoteWindowSize(int foo){ this.rwsize+=foo; }
//		internal void setRemotePacketSize(int foo){ this.rmpsize=foo; }
//
//		public virtual void run()
//		{
//		}
//
//		internal virtual void write(byte[] foo) 
//		{
//			write(foo, 0, foo.Length);
//		}
//		internal virtual void write(byte[] foo, int s, int l) 
//		{
//			//if(eof_remote)return;
//			if(io.outs!=null)
//				io.put(foo, s, l);
//		}
//		internal  void write_ext(byte[] foo, int s, int l)  
//		{
//			//if(eof_remote)return;
//			if(io.out_ext!=null)
//				io.put_ext(foo, s, l);
//		}
//
//		internal  void eof()
//		{
//			//System.out.println("EOF!!!! "+this);
//			//Thread.dumpStack();
//			if(eof_local)return;
//			eof_local=true;
//			//close=eof;
//			try
//			{
//				Buffer buf=new Buffer(100);
//				Packet packet=new Packet(buf);
//				packet.reset();
//				buf.putByte((byte)Session.SSH_MSG_CHANNEL_EOF);
//				buf.putInt(getRecipient());
//				session.write(packet);
//			}
//			catch
//			{
//				//System.out.println("Channel.eof");
//				//e.printStackTrace();
//			}
//			if(!isConnected())
//			{
//				disconnect();
//			}
//		}
//
//		internal void close()
//		{
//			//System.out.println("close!!!!");
//			if(_close)return;
//			_close=true;
//			try
//			{
//				Buffer buf=new Buffer(100);
//				Packet packet=new Packet(buf);
//				packet.reset();
//				buf.putByte((byte)Session.SSH_MSG_CHANNEL_CLOSE);
//				buf.putInt(getRecipient());
//				session.write(packet);
//				session.disconnect();
//			}
//			catch
//			{
//				//e.printStackTrace();
//			}
//		}
//		internal static void eof(Session session)
//		{
//			Channel[] channels=null;
//			int count=0;
//			lock(pool)
//			{
//				channels=new Channel[pool.Count];
//				for(int i=0; i<pool.Count; i++)
//				{
//					try
//					{
//						Channel c=((Channel)(pool[i]));
//						if(c.session==session)
//						{
//							channels[count++]=c;
//						}
//					}
//					catch
//					{
//					}
//				} 
//			}
//			for(int i=0; i<count; i++)
//			{
//				channels[i].eof();
//			}
//		}
//
//		//public void finalize() {
//		public virtual void finalize() 
//		{
//			disconnect();
//			//super.finalize();
//			session=null;
//		}
//
//		public virtual void disconnect()
//		{
//			//System.out.println(this+":disconnect "+((ChannelExec)this).command+" "+io.in);
//			//System.out.println(this+":disconnect "+io+" "+io.in);
//			close();
//			//System.out.println("$1");
//    
//			thread=null;
//			try
//			{
//				if(io!=null)
//				{
//					try
//					{
//						//System.out.println(" io.in="+io.in);
//						if(io.ins!=null && 
//							(io.ins is PassiveInputStream)
//							)
//							io.ins.Close();
//					}
//					catch{}
//					try
//					{
//						//System.out.println(" io.out="+io.out);
//						if(io.outs!=null && 
//							(io.outs is PassiveOutputStream)
//							)
//							io.outs.Close();
//					}
//					catch{}
//				}
//			}
//			catch
//			{
//				//e.printStackTrace();
//			}
//			//System.out.println("$2");
//			io=null;
//			Channel.del(this);
//		}
//
//		public bool isConnected()
//		{
//			if(this.session!=null)
//			{
//				return session.IsConnected();
//			}
//			return false;
//		}
//
//		public void sendSignal(String foo)  
//		{
//			RequestSignal request=new RequestSignal();
//			request.setSignal(foo);
//			request.request(session, this);
//		}
//
//		//  public String toString(){
//		//      return "Channel: type="+new String(type)+",id="+id+",recipient="+recipient+",window_size="+window_size+",packet_size="+packet_size;
//		//  }
//
//		/*
//		  class OutputThread extends Thread{
//			Channel c;
//			OutputThread(Channel c){ this.c=c;}
//			public void run(){c.output_thread();}
//		  }
//		*/
//
//		internal class PassiveInputStream : PipedInputStream
//		{
//			PipedOutputStream outs;
//			internal PassiveInputStream(PipedOutputStream outs) :base(outs)
//			{      
//				this.outs=outs;
//			}
//			public override void close()
//			{
//				if(outs!=null)
//				{
//					this.outs.close();
//				}
//				outs=null;
//			}
//		}
//		internal class PassiveOutputStream : PipedOutputStream
//		{
//			internal PassiveOutputStream(PipedInputStream ins):base(ins) 
//			{
//      
//			}
//		}
//
//		internal void setExitStatus(int foo){ exitstatus=foo; }
//		public int getExitStatus(){ return exitstatus; }
//
//		internal void setSession(Session session)
//		{
//			this.session=session;
//		}
//	}
//	
//	public class MyPipedInputStream : Tamir.Streams.PipedInputStream
//	{
//		public MyPipedInputStream():base(){}
//
//		public MyPipedInputStream(int size):base()
//		{
//			buffer=new byte[size];
//		}
//		public MyPipedInputStream(PipedOutputStream outs):base(outs) { }
//		public MyPipedInputStream(PipedOutputStream outs, int size):base(outs)
//		{
//			buffer=new byte[size];
//		}
//	}
//}
