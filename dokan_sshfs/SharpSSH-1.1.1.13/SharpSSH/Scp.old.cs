//using System;
//using Tamir.SharpSsh.jsch;
//using System.IO;
//using System.Windows.Forms;
//using System.Text;
//using System.Collections;
//
///* 
// * Scp.cs
// * 
// * THIS SOURCE CODE IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY
// * KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
// * IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A PARTICULAR
// * PURPOSE.
// * 
// * Copyright (C) 2005 Tamir Gal, tamirgal@myrealbox.com.
// */
//
//namespace Tamir.SharpSsh
//{
//	/// <summary>
//	/// Class for handling SCP file transfers over SSH connection.
//	/// </summary>
//	public class Scp
//	{
//		/// <summary>
//		/// Triggered when this SCP object starts connecting to the remote server.
//		/// </summary>
//		public event FileTansferEvent OnConnecting;
//		/// <summary>
//		/// Triggered when this SCP object starts the file transfer process.
//		/// </summary>
//		public event FileTansferEvent OnStart;
//		/// <summary>
//		/// Triggered when this SCP object ends the file transfer process.
//		/// </summary>
//		public event FileTansferEvent OnEnd;
//		/// <summary>
//		/// Triggered on every interval with the transfer progress iformation.
//		/// </summary>
//		public event FileTansferEvent OnProgress;
//
//		/// <summary>
//		/// The default value of the progress update interval.
//		/// </summary>
//		private int m_interval = 250;
//
//		/// <summary>
//		/// Copies a file from local machine to a remote SSH machine.
//		/// </summary>
//		/// <param name="localFile">The local file path.</param>
//		/// <param name="remoteHost">The remote machine's hostname or IP address</param>
//		/// <param name="remoteFile">The path of the remote file.</param>
//		/// <param name="user">The username for the connection.</param>
//		/// <param name="pass">The password for the connection.</param>
//		public void To(string localFile, string remoteHost,string remoteFile, string user, string pass)
//		{
//			Channel channel=null;
//			int filesize=0;
//			int copied=0;
//			try
//			{
//				double progress=0;
//				SendConnectingMessage("Connecting to "+remoteHost+"...");
//
//				JSch jsch=new JSch();
//				Session session=jsch.getSession(user, remoteHost, 22);
//				session.setPassword( pass );
//
//				Hashtable config=new Hashtable();
//				config.Add("StrictHostKeyChecking", "no");
//				session.setConfig(config);
//
//				session.connect();
//
//				// exec 'scp -t rfile' remotely
//				String command="scp -p -t \""+remoteFile+"\"";
//				channel=session.openChannel("exec");
//				((ChannelExec)channel).setCommand(command);
//
//				// get I/O streams for remote scp
//				Stream outs=channel.getOutputStream();
//				Stream ins=channel.getInputStream();
//
//				channel.connect();
//
//				SendStartMessage("Connected, starting transfer.");
//
//				byte[] tmp=new byte[1];
//
//				if(checkAck(ins)!=0)
//				{
//					throw new Exception("Error openning communication channel.");
//				}
//
//				// send "C0644 filesize filename", where filename should not include '/'
//							
//				filesize=(int)(new FileInfo(localFile)).Length;
//				command="C0644 "+filesize+" ";
//				if(localFile.LastIndexOf('/')>0)
//				{
//					command+=localFile.Substring(localFile.LastIndexOf('/')+1);
//				}
//				else
//				{
//					command+=localFile;
//				}
//				command+="\n";
//				byte[] buff = Util.getBytes(command);
//				outs.Write(buff, 0, buff.Length); outs.Flush();
//
//				if(checkAck(ins)!=0)
//				{
//					throw new Exception("Error openning communication channel.");				
//				}
//
//				// send a content of lfile
//				SendProgressMessage(0, filesize, "Transferring...");
//				FileStream fis=File.OpenRead(localFile);
//				byte[] buf=new byte[1024];
//				copied = 0;
//				while(true)
//				{
//					int len=fis.Read(buf, 0, buf.Length);
//					if(len<=0) break;
//					outs.Write(buf, 0, len); outs.Flush();
//					copied += len;
//					progress = (copied*100.0/filesize);
//					SendProgressMessage(copied, filesize, "Transferring...");
//				}
//				fis.Close();
//
//				// send '\0'
//				buf[0]=0; outs.Write(buf, 0, 1); outs.Flush();
//			
//				SendProgressMessage(copied, filesize, "Verifying transfer...");
//				if(checkAck(ins)!=0)
//				{
//					throw new Exception("Unknow error during file transfer.");				
//				}
//				SendEndMessage(copied, filesize, "Transfer completed successfuly ("+copied+" bytes).");
//				try{channel.close();}
//				catch{}
//			}
//			catch(Exception e)
//			{
//				SendEndMessage(copied,filesize, "Transfer ended with an error.");
//				try{channel.close();}
//				catch{}
//				throw e;
//			}
//		}
//
//		/// <summary>
//		/// Copies a file from a remote SSH machine to the local machine.
//		/// </summary>
//		/// <param name="remoteHost">The remote machine's hosname or IP address.</param>
//		/// <param name="remoteFile">The remote file path.</param>
//		/// <param name="user">The username or the connection.</param>
//		/// <param name="pass">The password for the connection.</param>
//		/// <param name="localFile">The local file path.</param>
//		public void From(string remoteHost,string remoteFile, string user, string pass, string localFile)
//		{
//			Channel channel=null;
//			int filesize=0;
//			int copied=0;
//			try
//			{
//				String prefix=null;
//				if(Directory.Exists(localFile))
//				{
//					prefix=localFile+Path.DirectorySeparatorChar;
//				}
//
//				double progress=0;
//				SendConnectingMessage("Connecting to "+remoteHost+"...");
//
//				JSch jsch=new JSch();
//				Session session=jsch.getSession(user, remoteHost, 22);
//				session.setPassword( pass );
//
//				Hashtable config=new Hashtable();
//				config.Add("StrictHostKeyChecking", "no");
//				session.setConfig(config);
//
//				session.connect();
//
//				// exec 'scp -f rfile' remotely
//				String command="scp -f \""+remoteFile + "\"";
//				channel=session.openChannel("exec");
//				((ChannelExec)channel).setCommand(command);
//
//				// get I/O streams for remote scp
//				Stream outs=channel.getOutputStream();
//				Stream ins=channel.getInputStream();
//
//				channel.connect();
//
//				SendStartMessage("Connected, starting transfer.");
//
//				byte[] buf=new byte[1024];
//
//				// send '\0'
//				buf[0]=0; outs.Write(buf, 0, 1); outs.Flush();
//				int c=checkAck(ins);
//				if(c!='C')
//				{
//					throw new Exception("Error openning communication channel.");
//				}
//
//				// read '0644 '
//				ins.Read(buf, 0, 5);
//
//				filesize=0;
//				while(true)
//				{
//					ins.Read(buf, 0, 1);
//					if(buf[0]==' ')break;
//					filesize=filesize*10+(buf[0]-'0');
//				}
//
//				String file=null;
//				for(int i=0;;i++)
//				{
//					ins.Read(buf, i, 1);
//					if(buf[i]==(byte)0x0a)
//					{
//						file=Util.getString(buf, 0, i);
//						break;
//					}
//				}
//
//				// send '\0'
//				buf[0]=0; outs.Write(buf, 0, 1); outs.Flush();
//
//				// read a content of lfile
//				FileStream fos=File.OpenWrite(prefix==null ? 
//				localFile :
//					prefix+file);
//				int foo;
//				int size=filesize;
//				copied=0;
//				while(true)
//				{
//					if(buf.Length<filesize) foo=buf.Length;
//					else foo=filesize;
//					int len=ins.Read(buf, 0, foo);
//					copied += len;
//					fos.Write(buf, 0, foo);
//					progress += (len*100.0/size);
//					SendProgressMessage(copied, size, "Transferring...");
//					filesize-=foo;
//					if(filesize==0) break;
//				}
//				fos.Close();
//
//				byte[] tmp=new byte[1];
//
//				SendProgressMessage(copied, size, "Verifying transfer...");
//				if(checkAck(ins)!=0)
//				{
//					throw new Exception("Unknow error during file transfer.");
//				}
//
//				// send '\0'
//				buf[0]=0; outs.Write(buf, 0, 1); outs.Flush();
//				SendEndMessage(copied, size, "Transfer completed successfuly ("+copied+" bytes).");
//			}
//			catch(Exception e)
//			{
//				SendEndMessage(copied,filesize, "Transfer ended with an error.");
//				try{channel.close();}
//				catch{}
//				throw e;
//			}
//		}
//
//		private int checkAck(Stream ins) 
//		{
//			int b=ins.ReadByte();
//			// b may be 0 for success,
//			//          1 for error,
//			//          2 for fatal error,
//			//          -1
//			if(b==0) return b;
//			if(b==-1) return b;
//
//			if(b==1 || b==2)
//			{
//				StringBuilder sb=new StringBuilder();
//				int c;
//				do 
//				{
//					c=ins.ReadByte();
//					sb.Append((char)c);
//				}
//				while(c!='\n');
//				if(b==1)
//				{ // error
//					throw new Exception(sb.ToString());
//				}
//				if(b==2)
//				{ // fatal error
//					throw new Exception(sb.ToString());
//				}
//			}
//			return b;
//		}
//
//		private void SendConnectingMessage(string msg)
//		{
//			if(OnConnecting != null)
//				OnConnecting(-1, 0, msg);
//		}
//
//		private void SendStartMessage(string msg)
//		{
//			if(OnStart != null)
//				OnStart(-1, 0, msg);
//		}
//
//		private void SendEndMessage(int transferredBytes, int totalBytes, string msg)
//		{
//			if(OnEnd != null)
//				OnEnd(transferredBytes, totalBytes, msg);
//		}
//
//		DateTime lastUpdate = DateTime.Now;
//		private void SendProgressMessage(int transferredBytes, int totalBytes, string msg)
//		{
//			if(OnProgress != null)
//			{
//				TimeSpan diff = DateTime.Now-lastUpdate;
//
//				if(diff.Milliseconds>ProgressUpdateInterval)
//				{
//					OnProgress(transferredBytes,totalBytes, msg);
//					lastUpdate=DateTime.Now;
//				}				
//			}
//		}
//
//		/// <summary>
//		/// Gets or sets the progress update interval in milliseconds
//		/// </summary>
//		public int ProgressUpdateInterval
//		{
//			get{return m_interval;}
//			set{m_interval=value;}
//		}
//	}
//
//	public delegate void FileTansferEvent(int transferredBytes, int totalBytes, string message);
//}
