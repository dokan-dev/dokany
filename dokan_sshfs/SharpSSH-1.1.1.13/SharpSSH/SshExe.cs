using System;
using Tamir.SharpSsh.jsch;
using System.Text;

/* 
 * SshExe.cs
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
	/// Summary description for SshExe.
	/// </summary>
	public class SshExec : SshBase
	{
		public SshExec(string host, string user, string password)
			: base(host, user, password)
		{
		}

		public SshExec(string host, string user)
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

		protected ChannelExec GetChannelExec(string command)
		{
			ChannelExec exeChannel = (ChannelExec)m_session.openChannel("exec"); 
			exeChannel.setCommand(command);
			return exeChannel;
		}

		public string RunCommand(string command)
		{
			m_channel = GetChannelExec(command);
			System.IO.Stream s = m_channel.getInputStream();
			m_channel.connect();
			byte[] buff = new byte[1024];
			StringBuilder res = new StringBuilder();
			int c = 0;
			while(true)
			{
				c = s.Read(buff, 0, buff.Length);
				if(c==-1) break;
				res.Append( Encoding.ASCII.GetString(buff, 0, c) );
				//Console.WriteLine(res);
			}
			m_channel.disconnect();
			return res.ToString();
		}

		public int RunCommand(string command, ref string StdOut, ref string StdErr)
		{
			StdOut = "";
			StdErr = "";
			m_channel = GetChannelExec(command);
			System.IO.Stream stdout = m_channel.getInputStream();
			System.IO.Stream stderr = ((ChannelExec)m_channel).getErrStream();
			m_channel.connect();
			byte[] buff = new byte[1024];
			StringBuilder sbStdOut = new StringBuilder();
			StringBuilder sbStdErr = new StringBuilder();
			int o=0; int e=0;
			while(true)
			{
				if(o!=-1) o = stdout.Read(buff, 0, buff.Length);
				if(o!=-1) StdOut += sbStdOut.Append(Encoding.ASCII.GetString(buff, 0, o));
				if(e!=-1) e = stderr.Read(buff, 0, buff.Length);
				if(e!=-1) StdErr += sbStdErr.Append(Encoding.ASCII.GetString(buff, 0, e));
				if((o==-1)&&(e==-1)) break;
			}
			m_channel.disconnect();

			return m_channel.getExitStatus();
		}

		public ChannelExec ChannelExec
		{
			get{return (ChannelExec)this.m_channel;}
		}
	}
}
