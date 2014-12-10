using System;
using Tamir.SharpSsh.jsch;
using System.IO;
using System.Text;
using System.Collections;

/* 
 * Scp.cs
 * 
 * Copyright (c) 2006 Tamir Gal, http://www.tamirgal.com, All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  	1. Redistributions of source code must retain the above copyright notice,
 *		this list of conditions and the following disclaimer.
 *
 *	    2. Redistributions in binary form must reproduce the above copyright 
 *		notice, this list of conditions and the following disclaimer in 
 *		the documentation and/or other materials provided with the distribution.
 *
 *	    3. The names of the authors may not be used to endorse or promote products
 *		derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR
 *  *OR ANY CONTRIBUTORS TO THIS SOFTWARE BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 **/

namespace Tamir.SharpSsh
{
	/// <summary>
	/// Class for handling SCP file transfers over SSH connection.
	/// </summary>
	public class Scp : SshTransferProtocolBase
	{
		private bool m_recursive = false;
		private bool m_verbos = false;
		private bool m_cancelled = false;

		public Scp(string host, string user, string password)
			: base(host, user, password)
		{
		}

		public Scp(string host, string user)
			: base(host, user)
		{
		}

		protected override string ChannelType
		{
			get { return "exec"; }
		}

		/// <summary>
		///This function is empty, so no channel is connected
		///on session connect 
		/// </summary>
		protected override void ConnectChannel()
		{
		}

		/// <summary>
		/// Gets or sets a value indicating the default recursive transfer behaviour
		/// </summary>
		public bool Recursive
		{
			get{return m_recursive;}
			set{m_recursive=value;}
		}

		/// <summary>
		/// Gets or sets a value indicating whether trace information should be printed
		/// </summary>
		public bool Verbos
		{
			get{return m_verbos;}
			set{m_verbos=value;}
		}

		public override void Cancel()
		{
			m_cancelled = true;
		}

		/// <summary>
		/// Creates a directory on the remot server
		/// </summary>
		/// <param name="dir">The new directory</param>
		public override void Mkdir(string dir)
		{
			SCP_CheckConnectivity();

			Channel channel=null;
			Stream server = null;
			m_cancelled=false;

			SCP_ConnectTo(out channel, out server, dir, true);
			SCP_EnterIntoDir(server, dir);
			channel.disconnect();
		}

		public override void Put(string fromFilePath, string toFilePath)
		{
			this.To(fromFilePath, toFilePath);
		}

		public override void Get(string fromFilePath, string toFilePath)
		{
			this.From(fromFilePath, toFilePath);
		}



		/// <summary>
		/// Copies a file from local machine to a remote SSH machine.
		/// </summary>
		/// <param name="localPath">The local file path.</param>
		/// <param name="remotePath">The path of the remote file.</param>
		public void To(string localPath, string remotePath)
		{
			this.To(localPath, remotePath, Recursive);
		}


		/// <summary>
		/// Copies a file from local machine to a remote SSH machine.
		/// </summary>
		/// <param name="localPath">The local file path.</param>
		/// <param name="remotePath">The path of the remote file.</param>
		public void To(string localPath, string remotePath, bool _recursive)
		{
			SCP_CheckConnectivity();

			Channel channel=null;
			Stream server = null;
			m_cancelled=false;
 
			try
			{
				//if we are sending a single file
				if(File.Exists(localPath))
				{
					SCP_ConnectTo(out channel, out server, remotePath, _recursive);
					SCP_SendFile(server, localPath, remotePath);					
					channel.disconnect();
				}
				//else, if we are sending a local directory
				else if(Directory.Exists(localPath))
				{
					if(!_recursive)
					{
						throw new SshTransferException(Path.GetFileName("'"+localPath)+"' is a directory, you should use recursive transfer.");
					}
					SCP_ConnectTo(out channel, out server, remotePath, true);
					ToRecursive(server, localPath, remotePath);
					channel.disconnect();
				}
				else
				{
					throw new SshTransferException("File not found: "+localPath);
				}
			}
			catch(Exception e)
			{
				if(Verbos)
					Console.WriteLine("Error: "+e.Message);				
				//SendEndMessage(remoteFile, localFile, filesize,filesize, "Transfer ended with an error.");
				try{channel.disconnect();}
				catch{}
				throw e;
			}
		}

		/// <summary>
		/// Copies files and directories from local machine to a remote SSH machine using SCP.
		/// </summary>
		/// <param name="server">I/O Stream for the remote server</param>
		/// <param name="src">Source to copy</param>
		/// <param name="dst">Destination path</param>
		private void ToRecursive(Stream server, string src, string dst)
		{
			if(Directory.Exists(src))
			{
				SCP_EnterIntoDir(server, Path.GetFileName(dst));
				foreach(string file in Directory.GetFiles(src))
				{
					SCP_SendFile(server, file, Path.GetFileName( file));
				}
				if(m_cancelled)
				{
					return;
				}
				foreach(string dir in Directory.GetDirectories(src))
				{
					ToRecursive(server, dir, Path.GetFileName(dir));
				}
				SCP_EnterIntoParent(server);
			}
			else if(File.Exists(src))
			{
				SCP_SendFile(server, src, Path.GetFileName(src));
			}
			else
			{
				throw new SshTransferException("File not found: "+src);
			}
		}

		/// <summary>
		/// Copies a file from a remote SSH machine to the local machine using SCP.
		/// </summary>
		/// <param name="remoteFile">The remmote file name</param>
		/// <param name="localPath">The local destination path</param>
		public void From(string remoteFile, string localPath)
		{
			this.From(remoteFile, localPath, Recursive);
		}

		/// <summary>
		/// Copies a file from a remote SSH machine to the local machine using SCP.
		/// </summary>
		/// <param name="remoteFile">The remmote file name</param>
		/// <param name="localPath">The local destination path</param>
		/// <param name="recursive">Value indicating whether a recursive transfer should take place</param>
		public void From(string remoteFile, string localPath, bool _recursive)
		{
			SCP_CheckConnectivity();

			Channel channel=null;
			Stream server = null;
			m_cancelled=false;
			int filesize=0;
			String filename=null;
			string cmd = null;

			try
			{
				String dir=null;
				if(Directory.Exists(localPath))
				{
					dir= Path.GetFullPath( localPath );
				}

				SCP_ConnectFrom(out channel, out server, remoteFile, _recursive);				

				byte[] buf=new byte[1024];

				// send '\0'
				SCP_SendAck(server);
				int c=SCP_CheckAck(server);
				
				//parse scp commands
				while((c=='D')||(c=='C')||(c=='E'))
				{
					if(m_cancelled)
						break;

					cmd = ""+(char)c;
					if(c=='E')
					{
						c=SCP_CheckAck(server);
						dir = Path.GetDirectoryName(dir);
						if(Verbos) Console.WriteLine("E");
						//send '\0'
						SCP_SendAck(server);
						c=(char)SCP_CheckAck(server);
						continue;
					}
					
					// read '0644 ' or '0755 '
					server.Read(buf, 0, 5);
					for(int i=0;i<5;i++)
						cmd+=(char)buf[i];
					
					//reading file size
					filesize=0;
					while(true)
					{
						server.Read(buf, 0, 1);
						if(buf[0]==' ')break;
						filesize=filesize*10+(buf[0]-'0');
					}					

					//reading file name					
					for(int i=0;;i++)
					{
						server.Read(buf, i, 1);
						if(buf[i]==(byte)0x0a)
						{
							filename=Util.getString(buf, 0, i);							
							break;
						}
					}
					cmd += " "+filesize+" "+filename;
					// send '\0'
					SCP_SendAck(server);

					//Receive file
					if(c=='C')
					{
						if(Verbos) Console.WriteLine("Sending file modes: "+cmd);
						SCP_ReceiveFile(server, remoteFile, 
							dir==null?localPath:dir+"/"+filename,
							filesize);

						if(m_cancelled)
							break;
						
						// send '\0'
						SCP_SendAck(server);
					}
					//Enter directory
					else if(c=='D')
					{
						if(dir==null)
						{
							if(File.Exists(localPath)) throw new SshTransferException("'"+localPath+"' is not a directory");
							dir = localPath;
							Directory.CreateDirectory(dir);
						}
						if(Verbos) Console.WriteLine("Entering directory: "+cmd);
						dir += "/"+filename;
						Directory.CreateDirectory(dir);
					}

					c=SCP_CheckAck(server);
				}				
				channel.disconnect();
			}
			catch(Exception e)
			{
				if(Verbos)
					Console.WriteLine("Error: "+e.Message);				
				try
				{
					channel.disconnect();
				}
				catch{}
				throw e;
			}
		}

		#region SCP private functions

		/// <summary>
		/// Checks is a channel is already connected by this instance
		/// </summary>
		protected void SCP_CheckConnectivity()
		{
			if(!Connected)
				throw new Exception("Channel is down.");
		}

		/// <summary>
		/// Connect a channel to the remote server using the 'SCP TO' command ('scp -t')
		/// </summary>
		/// <param name="channel">Will contain the new connected channel</param>
		/// <param name="server">Will contaun the new connected server I/O stream</param>
		/// <param name="rfile">The remote path on the server</param>
		/// <param name="recursive">Idicate a recursive scp transfer</param>
		protected void SCP_ConnectTo(out Channel channel, out Stream server, string rfile, bool recursive)
		{
			string scpCommand = "scp -p -t ";
			if(recursive) scpCommand += "-r ";
			scpCommand += "\""+rfile+"\"";

			channel = (ChannelExec)m_session.openChannel(ChannelType); 
			((ChannelExec)channel).setCommand(scpCommand);

			server = 
				new Tamir.Streams.CombinedStream
				(channel.getInputStream(), channel.getOutputStream());
			channel.connect();

			SCP_CheckAck(server);
		}

		/// <summary>
		/// Connect a channel to the remote server using the 'SCP From' command ('scp -f')
		/// </summary>
		/// <param name="channel">Will contain the new connected channel</param>
		/// <param name="server">Will contaun the new connected server I/O stream</param>
		/// <param name="rfile">The remote path on the server</param>
		/// <param name="recursive">Idicate a recursive scp transfer</param>
		protected void SCP_ConnectFrom(out Channel channel, out Stream server, string rfile, bool recursive)
		{
			string scpCommand = "scp -f ";
			if(recursive) scpCommand += "-r ";
			scpCommand += "\""+rfile+"\"";

			channel = (ChannelExec)m_session.openChannel(ChannelType); 
			((ChannelExec)channel).setCommand(scpCommand);

			server = 
				new Tamir.Streams.CombinedStream
				(channel.getInputStream(), channel.getOutputStream());
			channel.connect();

			//SCP_CheckAck(server);
		}

		/// <summary>
		/// Transfer a file to the remote server
		/// </summary>
		/// <param name="server">A connected server I/O stream</param>
		/// <param name="src">The source file to copy</param>
		/// <param name="dst">The remote destination path</param>
		protected void SCP_SendFile(Stream server, string src, string dst)
		{
			int filesize = 0;
			int copied = 0;

			filesize=(int)(new FileInfo(src)).Length;				

			byte[] tmp=new byte[1];

			// send "C0644 filesize filename", where filename should not include '/'
							
			string command="C0644 "+filesize+" "+Path.GetFileName(dst)+"\n";
			if(Verbos) Console.WriteLine("Sending file modes: "+command);
			SendStartMessage(src, dst, filesize, "Starting transfer.");
			byte[] buff = Util.getBytes(command);
			server.Write(buff, 0, buff.Length); server.Flush();

			if(SCP_CheckAck(server)!=0)
			{
				throw new SshTransferException("Error openning communication channel.");				
			}

			// send a content of lfile
			SendProgressMessage(src, dst, copied, filesize, "Transferring...");
			FileStream fis=File.OpenRead(src);
			byte[] buf=new byte[1024*10*2];

			while(!m_cancelled)
			{
				int len=fis.Read(buf, 0, buf.Length);
				if(len<=0) break;
				server.Write(buf, 0, len); server.Flush();
				copied += len;
				SendProgressMessage(src, dst, copied, filesize, "Transferring...");
			}
			fis.Close();

			if(m_cancelled)
				return;

			// send '\0'
			buf[0]=0; server.Write(buf, 0, 1); server.Flush();
		
			SendProgressMessage(src, dst, copied, filesize, "Verifying transfer...");
			if(SCP_CheckAck(server)!=0)
			{
				SendEndMessage(src, dst,copied,filesize, "Transfer ended with an error.");
				throw new SshTransferException("Unknow error during file transfer.");				
			}
			SendEndMessage(src, dst, copied, filesize, "Transfer completed successfuly ("+copied+" bytes).");
		}

		/// <summary>
		/// Transfer a file from the remote server
		/// </summary>
		/// <param name="server">A connected server I/O stream</param>
		/// <param name="rfile">The remote file to copy</param>
		/// <param name="lfile">The local destination path</param>
		protected void SCP_ReceiveFile(Stream server, string rfile, string lfile, int size)
		{
			int copied = 0;
			SendStartMessage(rfile, lfile, size, "Connected, starting transfer.");
			// read a content of lfile
			FileStream fos=File.OpenWrite(lfile);
			int foo;
			int filesize=size;
			byte[] buf = new byte[1024];
			while(!m_cancelled)
			{
				if(buf.Length<filesize) foo=buf.Length;
				else foo=filesize;
				int len=server.Read(buf, 0, foo);
				copied += len;
				fos.Write(buf, 0, foo);
				SendProgressMessage(rfile, lfile, copied, size, "Transferring...");
				filesize-=foo;
				if(filesize==0) break;
			}
			fos.Close();
			if(m_cancelled)
				return;
			SCP_CheckAck(server);
			SendEndMessage(rfile, lfile, copied, size, "Transfer completed successfuly ("+filesize+" bytes).");
		}

		/// <summary>
		/// Instructs the remote server to enter into a directory
		/// </summary>
		/// <param name="server">A connected server I/O stream</param>
		/// <param name="dir">The directory name/param>
		protected void SCP_EnterIntoDir(Stream server, string dir)
		{
			try
			{
				byte[] tmp=new byte[1];

				// send "C0644 filesize filename", where filename should not include '/'
								
				string command="D0755 0 "+Path.GetFileName(dir)+"\n";
				if(Verbos) Console.WriteLine("Enter directory: "+command);
				
				byte[] buff = Util.getBytes(command);
				server.Write(buff, 0, buff.Length); server.Flush();

				if(SCP_CheckAck(server)!=0)
				{
					throw new SshTransferException("Error openning communication channel.");				
				}
			}
			catch{}
		}

		/// <summary>
		/// Instructs the remote server to go up one level
		/// </summary>
		/// <param name="server">A connected server I/O stream</param>
		protected void SCP_EnterIntoParent(Stream server)
		{
			try
			{
				byte[] tmp=new byte[1];

				// send "C0644 filesize filename", where filename should not include '/'
								
				string command="E\n";
				if(Verbos) Console.WriteLine(command);
				
				byte[] buff = Util.getBytes(command);
				server.Write(buff, 0, buff.Length); server.Flush();

				if(SCP_CheckAck(server)!=0)
				{
					throw new SshTransferException("Error openning communication channel.");				
				}
			}
			catch{}
		}

		/// <summary>
		/// Gets server acknowledgment
		/// </summary>
		/// <param name="ins">A connected server I/O stream</param>
		private int SCP_CheckAck(Stream ins) 
		{
			int b=ins.ReadByte();
			// b may be 0 for success,
			//          1 for error,
			//          2 for fatal error,
			//          -1
			if(b==0) return b;
			if(b==-1) return b;

			if(b==1 || b==2)
			{
				StringBuilder sb=new StringBuilder();
				int c;
				do 
				{
					c=ins.ReadByte();
					sb.Append((char)c);
				}
				while(c!='\n');
				if(b==1)
				{ // error
					//Console.WriteLine(sb.ToString());
					throw new SshTransferException(sb.ToString());
				}
				if(b==2)
				{ // fatal error
					//Console.WriteLine(sb.ToString());
					throw new SshTransferException(sb.ToString());
				}
			}
			return b;
		}

		/// <summary>
		/// Sends acknowledgment to remote server
		/// </summary>
		/// <param name="server">A connected server I/O stream</param>
		private void SCP_SendAck(Stream server)
		{
			server.WriteByte(0);
			server.Flush();
		}

		#endregion SCP private functions
	}
}
