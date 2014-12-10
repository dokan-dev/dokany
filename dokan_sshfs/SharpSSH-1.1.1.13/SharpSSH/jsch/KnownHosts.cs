using System;
using System.IO;

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

	public class KnownHosts : HostKeyRepository
	{
		private const String _known_hosts="known_hosts";
		/*
		static final int OK=0;
		static final int NOT_INCLUDED=1;
		static final int CHANGED=2;
		*/

		/*
		static final int SSHDSS=0;
		static final int SSHRSA=1;
		static final int UNKNOWN=2;
		*/

		private JSch jsch=null;
		private String known_hosts=null;
		private System.Collections.ArrayList pool=null;

		internal KnownHosts(JSch jsch) : base()
		{
			this.jsch=jsch;
			pool=new System.Collections.ArrayList();
		}

		internal void setKnownHosts(String foo) 
		{
			try
			{
				known_hosts=foo;
				FileStream fis=File.OpenRead(foo);
				setKnownHosts( new StreamReader(fis) );
			}
			catch
			{
			} 
		}
		internal void setKnownHosts(StreamReader foo) 
		{
			pool.Clear();
			System.Text.StringBuilder sb=new System.Text.StringBuilder();
			byte i;
			int j;
			bool error=false;
			try
			{
				StreamReader fis=foo;
				String host;
				String key=null;
				int type;
				byte[] buf=new byte[1024];
				int bufl=0;
			loop:
				while(true)
				{
					bufl=0;
					while(true)
					{
						j=fis.Read();
						if(j==-1){ goto break_loop;}
						if(j==0x0d){ continue; }
						if(j==0x0a){ break; }
						buf[bufl++]=(byte)j;
					}

					j=0;
					while(j<bufl)
					{
						i=buf[j];
						if(i==' '||i=='\t'){ j++; continue; }
						if(i=='#')
						{
							addInvalidLine(System.Text.Encoding.Default.GetString(buf, 0, bufl));			
							goto loop;
						}
						break;
					}
					if(j>=bufl)
					{ 
						addInvalidLine(System.Text.Encoding.Default.GetString(buf, 0, bufl));
						goto loop;
					}

					sb.Length = 0;
					while(j<bufl)
					{
						i=buf[j++];
						if(i==0x20 || i=='\t'){ break; }
						sb.Append((char)i);
					}
					host=sb.ToString();
					if(j>=bufl || host.Length==0)
					{
						addInvalidLine(System.Text.Encoding.Default.GetString(buf, 0, bufl));
						goto loop; 
					}

					sb.Length=0;
					type=-1;
					while(j<bufl)
					{
						i=buf[j++];
						if(i==0x20 || i=='\t'){ break; }
						sb.Append((char)i);
					}
					if(sb.ToString().Equals("ssh-dss")){ type=HostKey.SSHDSS; }
					else if(sb.ToString().Equals("ssh-rsa")){ type=HostKey.SSHRSA; }
					else { j=bufl; }
					if(j>=bufl)
					{
						addInvalidLine(Util.getString(buf, 0, bufl));
						goto loop; 
					}

					sb.Length=0;
					while(j<bufl)
					{
						i=buf[j++];
						if(i==0x0d){ continue; }
						if(i==0x0a){ break; }
						sb.Append((char)i);
					}
					key=sb.ToString();
					if(key.Length==0)
					{
						addInvalidLine(Util.getString(buf, 0, bufl));
						goto loop; 
					}

					//System.out.println(host);
					//System.out.println("|"+key+"|");

					HostKey hk = new HostKey(host, type, 
						Util.fromBase64(Util.getBytes(key), 0, 
						key.Length));
					pool.Add(hk);
				}

			break_loop:

				fis.Close();
				if(error)
				{
					throw new JSchException("KnownHosts: invalid format");
				}
			}
			catch(Exception e)
			{
				if(e is JSchException)
				{
					throw (JSchException)e;         
				}
				throw new JSchException(e.ToString());
			}
		}
		private void addInvalidLine(String line)
		{
			HostKey hk = new HostKey(line, HostKey.UNKNOWN, null);
			pool.Add(hk);
		}
		String getKnownHostsFile(){ return known_hosts; }
		public override String getKnownHostsRepositoryID(){ return known_hosts; }

		public override int check(String host, byte[] key)
		{
			//String foo; 
			//byte[] bar;
			HostKey hk;
			int result=NOT_INCLUDED;
			int type=getType(key);
			for(int i=0; i<pool.Count; i++)
			{
				hk=(HostKey)(pool[i]);
				if(isIncluded(hk.host, host) && hk.type==type)
				{
					if(Util.array_equals(hk.key, key))
					{
						//System.out.println("find!!");
						return OK;
					}
					else
					{
						result=CHANGED;
					}
				}
			}
			//System.out.println("fail!!");
			return result;
		}
		public override void add(String host, byte[] key, UserInfo userinfo)
		{
			HostKey hk;
			int type=getType(key);
			for(int i=0; i<pool.Count; i++)
			{
				hk=(HostKey)(pool[i]);
				if(isIncluded(hk.host, host) && hk.type==type)
				{
					/*
							if(Util.array_equals(hk.key, key)){ return; }
							if(hk.host.equals(host)){
							hk.key=key;
							return;
						}
						else{
							hk.host=deleteSubString(hk.host, host);
						break;
						}
					*/
				}
			}
			hk=new HostKey(host, type, key);
			pool.Add(hk);

			String bar=getKnownHostsRepositoryID();
			if(userinfo!=null && 
				bar!=null)
			{
				bool foo=true;
				FileInfo goo=new FileInfo(bar);
				if(!goo.Exists)
				{
					foo=false;
					if(userinfo!=null)
					{
						foo=userinfo.promptYesNo(
							bar+" does not exist.\n"+
							"Are you sure you want to create it?"
							);
						DirectoryInfo dir =goo.Directory;
						if(foo && dir!=null && !dir.Exists)
						{
							foo=userinfo.promptYesNo(
								"The parent directory "+dir.Name+" does not exist.\n"+
								"Are you sure you want to create it?"
								);
							if(foo)
							{
								try{dir.Create(); userinfo.showMessage(dir.Name+" has been succesfully created.\nPlease check its access permission.");}
								catch
								{
									userinfo.showMessage(dir.Name+" has not been created.");
									foo=false;
								}
							}
						}
						if(goo==null)foo=false;
					}
				}
				if(foo)
				{
					try
					{ 
						sync(bar); 
					}
					catch(Exception e){ Console.WriteLine("sync known_hosts: "+e); }
				}
			}
		}

		public override HostKey[] getHostKey()
		{
			return getHostKey(null, null);
		}
		public override HostKey[] getHostKey(String host, String type)
		{
			lock(pool)
			{
				int count=0;
				for(int i=0; i<pool.Count; i++)
				{
					HostKey hk=(HostKey)pool[i];
					if(hk.type==HostKey.UNKNOWN) continue;
					if(host==null || 
						(isIncluded(hk.host, host) && 
						(type==null || hk.getType().Equals(type))))
					{
						count++;
					}
				}
				if(count==0)return null;
				HostKey[] foo=new HostKey[count];
				int j=0;
				for(int i=0; i<pool.Count; i++)
				{
					HostKey hk=(HostKey)pool[i];
					if(hk.type==HostKey.UNKNOWN) continue;
					if(host==null || 
						(isIncluded(hk.host, host) && 
						(type==null || hk.getType().Equals(type))))
					{
						foo[j++]=hk;
					}
				}
				return foo;
			}
		}
		public override void  remove(String host, String type)
		{
			remove(host, type, null);
		}
		public override void remove(String host, String type, byte[] key)
		{
			bool _sync=false;
			for(int i=0; i<pool.Count; i++)
			{
				HostKey hk=(HostKey)(pool[i]);
				if(host==null ||
					(hk.getHost().Equals(host) && 
					(type==null || (hk.getType().Equals(type) &&
					(key==null || Util.array_equals(key, hk.key))))))
				{
					pool.Remove(hk);
					_sync=true;
				}
			}
			if(_sync)
			{
				try{sync();}
				catch{};
			}
		}

		private void sync() 
		{ 
			if(known_hosts!=null)
				sync(known_hosts); 
		}
		private void sync(String foo) 
		{
			if(foo==null) return;
			FileStream fos=File.OpenWrite(foo);
			dump(fos);
			fos.Close();
		}

		private static byte[] space= new byte[]{(byte)0x20};
		private static byte[] cr= System.Text.Encoding.Default.GetBytes("\n");
		void dump(FileStream outs) 
		{
			//StreamWriter outs = new StreamWriter(fs);
			try
			{
				HostKey hk;
				for(int i=0; i<pool.Count; i++)
				{
					hk=(HostKey)(pool[i]);
					//hk.dump(out);
					String host=hk.getHost();
					String type=hk.getType();
					if(type.Equals("UNKNOWN"))
					{
						Write(outs, Util.getBytes(host));
						Write(outs, cr);
						continue;
					}
					Write(outs, Util.getBytes(host));
					Write(outs, space);
					Write(outs, Util.getBytes(type));
					Write(outs, space);
					Write(outs, Util.getBytes(hk.getKey()));
					Write(outs, cr);
				}
				outs.Flush();
			}
			catch(Exception e)
			{
				Console.WriteLine(e);
			}
		}

		private void Write(Stream s, byte[] buff)
		{
			s.Write(buff, 0, buff.Length);
		}
		private int getType(byte[] key)
		{
			if(key[8]=='d') return HostKey.SSHDSS;
			if(key[8]=='r') return HostKey.SSHRSA;
			return HostKey.UNKNOWN;
		}
		private String deleteSubString(String hosts, String host)
		{
			int i=0;
			int hostlen=host.Length;
			int hostslen=hosts.Length;
			int j;
			while(i<hostslen)
			{
				j=hosts.IndexOf(',', i);
				if(j==-1) break;
				if(!host.Equals(hosts.Substring(i, j)))
				{
					i=j+1;	  
					continue;
				}
				return hosts.Substring(0, i)+hosts.Substring(j+1);
			}
			if(hosts.EndsWith(host) && hostslen-i==hostlen)
			{
				return hosts.Substring(0, (hostlen==hostslen) ? 0 :hostslen-hostlen-1);
			}
			return hosts;
		}
		private bool isIncluded(String hosts, String host)
		{
			int i=0;
			int hostlen=host.Length;
			int hostslen=hosts.Length;
			int j;
			while(i<hostslen)
			{
				j=hosts.IndexOf(',', i);
				if(j==-1)
				{
					if(hostlen!=hostslen-i) return false;
					return Util.regionMatches( hosts, true, i, host, 0, hostlen);
					//return hosts.substring(i).equals(host);
				}
				if(hostlen==(j-i))
				{
					if(Util.regionMatches( hosts,true, i, host, 0, hostlen)) return true;
					//if(hosts.substring(i, i+hostlen).equals(host)) return true;
				}
				i=j+1;
			}
			return false;
		}
		/*
		private static boolean equals(byte[] foo, byte[] bar){
			if(foo.length!=bar.length)return false;
			for(int i=0; i<foo.length; i++){
			if(foo[i]!=bar[i])return false;
			}
			return true;
		}
		*/

		/*
		private static final byte[] space={(byte)0x20};
		private static final byte[] sshdss="ssh-dss".getBytes();
		private static final byte[] sshrsa="ssh-rsa".getBytes();
		private static final byte[] cr="\n".getBytes();

		public class HostKey{
			String host;
			int type;
			byte[] key;
			HostKey(String host, int type, byte[] key){
			this.host=host; this.type=type; this.key=key;
			}
			void dump(OutputStream out) throws IOException{
			if(type==UNKNOWN){
			out.write(host.getBytes());
			out.write(cr);
			return;
			}
			out.write(host.getBytes());
			out.write(space);
			if(type==HostKey.SSHDSS){ out.write(sshdss); }
			else if(type==HostKey.SSHRSA){ out.write(sshrsa);}
			out.write(space);
			out.write(Util.toBase64(key, 0, key.length));
			out.write(cr);
			}

			public String getHost(){ return host; }
			public String getType(){
			if(type==SSHDSS){ return new String(sshdss); }
			if(type==SSHRSA){ return new String(sshrsa);}
			return "UNKNOWN";
			}
			public String getKey(){
			return new String(Util.toBase64(key, 0, key.length));
			}
			public String getFingerPrint(){
			HASH hash=null;
			try{
			Class c=Class.forName(jsch.getConfig("md5"));
			hash=(HASH)(c.newInstance());
			}
			catch(Exception e){ System.err.println("getFingerPrint: "+e); }
			return Util.getFingerPrint(hash, key);
			}
		}
		*/
	}

}
