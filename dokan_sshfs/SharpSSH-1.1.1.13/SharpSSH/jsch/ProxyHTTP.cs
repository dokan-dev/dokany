using Tamir.SharpSsh.java;
using Tamir.SharpSsh.java.io;
using Tamir.SharpSsh.java.util;
using Tamir.SharpSsh.java.net;
using Tamir.SharpSsh.java.lang;
using Tamir.SharpSsh.jsch;
using System.IO;

namespace Tamir.SharpSsh.jsch
{
	public class ProxyHTTP : Proxy
	{
		private static int DEFAULTPORT=80;
		private String proxy_host;
		private int proxy_port;
		private JStream ins;
		private JStream outs;
		private Socket socket;

		private String user;
		private String passwd;

		public ProxyHTTP(String proxy_host)
		{
			int port=DEFAULTPORT;
			String host=proxy_host;
			if(proxy_host.indexOf(':')!=-1)
			{
				try
				{
					host=proxy_host.substring(0, proxy_host.indexOf(':'));
					port=Integer.parseInt(proxy_host.substring(proxy_host.indexOf(':')+1));
				}
				catch(Exception e)
				{
				}
			}
			this.proxy_host=host;
			this.proxy_port=port;
		}
		public ProxyHTTP(String proxy_host, int proxy_port)
		{
			this.proxy_host=proxy_host;
			this.proxy_port=proxy_port;
		}
		public void setUserPasswd(String user, String passwd)
		{
			this.user=user;
			this.passwd=passwd;
		}
		public void connect(SocketFactory socket_factory, String host, int port, int timeout)
		{
			try
			{
				if(socket_factory==null)
				{
					socket=Util.createSocket(proxy_host, proxy_port, timeout);    
					ins= new JStream(socket.getInputStream());
					outs=new JStream(socket.getOutputStream());
				}
				else
				{
					socket=socket_factory.createSocket(proxy_host, proxy_port);
					ins=new JStream(socket_factory.getInputStream(socket));
					outs=new JStream(socket_factory.getOutputStream(socket));
				}
				if(timeout>0)
				{
					socket.setSoTimeout(timeout);
				}
				socket.setTcpNoDelay(true);

				outs.write(new String("CONNECT "+host+":"+port+" HTTP/1.0\r\n").getBytes());

				if(user!=null && passwd!=null)
				{
					byte[] _code=(user+":"+passwd).getBytes();
					_code=Util.toBase64(_code, 0, _code.Length);
					outs.write(new String("Proxy-Authorization: Basic ").getBytes());
					outs.write(_code);
					outs.write(new String("\r\n").getBytes());
				}

				outs.write(new String("\r\n").getBytes());
				outs.flush();

				int foo=0;

				StringBuffer sb=new StringBuffer();
				while(foo>=0)
				{
					foo=ins.read(); if(foo!=13){sb.append((char)foo);  continue;}
					foo=ins.read(); if(foo!=10){continue;}
					break;
				}
				if(foo<0)
				{
					throw new System.IO.IOException(); 
				}

				String response=sb.toString(); 
				String reason="Unknow reason";
				int code=-1;
				try
				{
					foo=response.indexOf(' ');
					int bar=response.indexOf(' ', foo+1);
					code=Integer.parseInt(response.substring(foo+1, bar));
					reason=response.substring(bar+1);
				}
				catch(Exception e)
				{
				}
				if(code!=200)
				{
					throw new System.IO.IOException("proxy error: "+reason);
				}

				/*
				while(foo>=0){
				  foo=in.read(); if(foo!=13) continue;
				  foo=in.read(); if(foo!=10) continue;
				  foo=in.read(); if(foo!=13) continue;      
				  foo=in.read(); if(foo!=10) continue;
				  break;
				}
				*/

				int count=0;
				while(true)
				{
					count=0;
					while(foo>=0)
					{
						foo=ins.read(); if(foo!=13){count++;  continue;}
						foo=ins.read(); if(foo!=10){continue;}
						break;
					}
					if(foo<0)
					{
						throw new System.IO.IOException();
					}
					if(count==0)break;
				}
			}
			catch(RuntimeException e)
			{
				throw e;
			}
			catch(Exception e)
			{
				try{ if(socket!=null)socket.close(); }
				catch(Exception eee)
				{
				}
				String message="ProxyHTTP: "+e.toString();
				throw e;
			}
		}
		public Stream getInputStream(){ return ins.s; }
		public Stream getOutputStream(){ return outs.s; }
		public Socket getSocket(){ return socket; }
		public void close()
		{
			try
			{
				if(ins!=null)ins.close();
				if(outs!=null)outs.close();
				if(socket!=null)socket.close();
			}
			catch(Exception e)
			{
			}
			ins=null;
			outs=null;
			socket=null;
		}
		public static int getDefaultPort()
		{
			return DEFAULTPORT;
		}
	}
}
