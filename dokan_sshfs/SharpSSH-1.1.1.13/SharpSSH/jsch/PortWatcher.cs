using System;
using System.IO;
using System.Threading;
using Tamir.SharpSsh.java.net;
using Tamir.SharpSsh.java.lang;
using InetAddress = Tamir.SharpSsh.java.net.InetAddress;
using String = Tamir.SharpSsh.java.String;

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

	class PortWatcher : Tamir.SharpSsh.java.lang.Runnable
	{
		private static Tamir.SharpSsh.java.util.Vector pool=new Tamir.SharpSsh.java.util.Vector();


		internal Session session;
		internal int lport;
		internal int rport;
		internal String host;
		internal InetAddress boundaddress;
		internal Runnable thread;
		internal ServerSocket ss;

		internal static String[] getPortForwarding(Session session)
		{
			java.util.Vector foo=new java.util.Vector();
			lock(pool)
			{
				for(int i=0; i<pool.size(); i++)
				{
					PortWatcher p=(PortWatcher)(pool.elementAt(i));
					if(p.session==session)
					{
						foo.addElement(p.lport+":"+p.host+":"+p.rport);
					}
				}
			}
			String[] bar=new String[foo.size()];
			for(int i=0; i<foo.size(); i++)
			{
				bar[i]=(String)(foo.elementAt(i));
			}
			return bar;
		}
		internal static PortWatcher getPort(Session session, String address, int lport)
		{
			InetAddress addr;
			try
			{
				addr=InetAddress.getByName(address);
			}
			catch(Exception uhe)
			{
				throw new JSchException("PortForwardingL: invalid address "+address+" specified.");
			}
			lock(pool)
			{
				for(int i=0; i<pool.size(); i++)
				{
					PortWatcher p=(PortWatcher)(pool.elementAt(i));
					if(p.session==session && p.lport==lport)
					{
						if(p.boundaddress.isAnyLocalAddress() ||
							p.boundaddress.equals(addr))
							return p;
					}
				}
				return null;
			}
		}
		internal static PortWatcher addPort(Session session, String address, int lport, String host, int rport, ServerSocketFactory ssf) 
		{
			if(getPort(session, address, lport)!=null)
			{
				throw new JSchException("PortForwardingL: local port "+ address+":"+lport+" is already registered.");
			}
			PortWatcher pw=new PortWatcher(session, address, lport, host, rport, ssf);
			pool.addElement(pw);
			return pw;
		}
		internal static void delPort(Session session, String address, int lport) 
		{
			PortWatcher pw=getPort(session, address, lport);
			if(pw==null)
			{
				throw new JSchException("PortForwardingL: local port "+address+":"+lport+" is not registered.");
			}
			pw.delete();
			pool.removeElement(pw);
		}
		internal static void delPort(Session session)
		{
			lock(pool)
			{
				PortWatcher[] foo=new PortWatcher[pool.size()];
				int count=0;
				for(int i=0; i<pool.size(); i++)
				{
					PortWatcher p=(PortWatcher)(pool.elementAt(i));
					if(p.session==session) 
					{
						p.delete();
						foo[count++]=p;
					}
				}
				for(int i=0; i<count; i++)
				{
					PortWatcher p=foo[i];
					pool.removeElement(p);
				}
			}
		}
		internal PortWatcher(Session session, 
			String address, int lport, 
			String host, int rport,
			ServerSocketFactory factory) 
		{
			this.session=session;
			this.lport=lport;
			this.host=host;
			this.rport=rport;
			try
			{
				boundaddress=InetAddress.getByName(address);
				ss=(factory==null) ? 
					new ServerSocket(lport, 0, boundaddress) :
					factory.createServerSocket(lport, 0, boundaddress);
			}
			catch(Exception e)
			{ 
				Console.WriteLine(e);
				throw new JSchException("PortForwardingL: local port "+address+":"+lport+" cannot be bound.");
			}
		}

		public void run()
		{
			Buffer buf=new Buffer(300); // ??
			Packet packet=new Packet(buf);
			thread=this;
			try
			{
				while(thread!=null)
				{
					Socket socket=ss.accept();
					socket.setTcpNoDelay(true);
					Stream In=socket.getInputStream();
					Stream Out=socket.getOutputStream();
					ChannelDirectTCPIP channel=new ChannelDirectTCPIP();
					channel.init();
					channel.setInputStream(In);
					channel.setOutputStream(Out);
					session.addChannel(channel);
					((ChannelDirectTCPIP)channel).setHost(host);
					((ChannelDirectTCPIP)channel).setPort(rport);
					((ChannelDirectTCPIP)channel).setOrgIPAddress(socket.getInetAddress().getHostAddress());
					((ChannelDirectTCPIP)channel).setOrgPort(socket.getPort());
					channel.connect();
					if(channel.exitstatus!=-1)
					{
					}
				}
			}
			catch(Exception e)
			{
				//System.out.println("! "+e);
			}

			delete();
		}

		internal void delete()
		{
			thread=null;
			try
			{ 
				if(ss!=null)ss.close();
				ss=null;
			}
			catch(Exception e)
			{
			}
		}
	}
}
