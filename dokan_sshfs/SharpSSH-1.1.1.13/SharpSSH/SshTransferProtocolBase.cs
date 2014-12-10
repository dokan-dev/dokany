using System;

/* 
 * SshTransferProtocolBase.cs
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
	/// Summary description for SshTransferProtocolBase.
	/// </summary>
	public abstract class SshTransferProtocolBase : SshBase, ITransferProtocol
	{
		public SshTransferProtocolBase(string host, string user, string password)
			: base(host, user, password)
		{
		}

		public SshTransferProtocolBase(string host, string user)
			: base(host, user)
		{
		}
		#region ITransferProtocol Members

		public abstract void Get(string fromFilePath, string toFilePath);
		public abstract void Put(string fromFilePath, string toFilePath);
		public abstract void Mkdir(string directory);
		public abstract void Cancel();

		/// <summary>
		/// Triggered when transfer is starting
		/// </summary>
		public event FileTransferEvent OnTransferStart;
		/// <summary>
		/// Triggered when transfer ends
		/// </summary>
		public event FileTransferEvent OnTransferEnd;
		/// <summary>
		/// Triggered on every interval with the transfer progress iformation.
		/// </summary>
		public event FileTransferEvent OnTransferProgress;

		/// <summary>
		/// Sends a notification that a file transfer has started
		/// </summary>
		/// <param name="src">The source file to transferred</param>
		/// <param name="dst">Transfer destination</param>
		/// <param name="totalBytes">Total bytes to transfer</param>
		/// <param name="msg">A transfer message</param>
		protected void SendStartMessage(string src, string dst, int totalBytes, string msg)
		{
			if (OnTransferStart != null)
				OnTransferStart(src, dst, 0, totalBytes, msg);
		}

		/// <summary>
		/// Sends a notification that a file transfer has ended
		/// </summary>
		/// <param name="src">The source file to transferred</param>
		/// <param name="dst">Transfer destination</param>
		/// <param name="transferredBytes">Transferred Bytes</param>
		/// <param name="totalBytes">Total bytes to transfer</param>
		/// <param name="msg">A transfer message</param>
		protected void SendEndMessage(string src, string dst, int transferredBytes, int totalBytes, string msg)
		{
			if (OnTransferEnd != null)
				OnTransferEnd(src, dst, transferredBytes, totalBytes, msg);
		}

		/// <summary>
		/// Sends a transfer progress notification
		/// </summary>
		/// <param name="src">The source file to transferred</param>
		/// <param name="dst">Transfer destination</param>
		/// <param name="transferredBytes">Transferred Bytes</param>
		/// <param name="totalBytes">Total bytes to transfer</param>
		/// <param name="msg">A transfer message</param>
		protected void SendProgressMessage(string src, string dst, int transferredBytes, int totalBytes, string msg)
		{
			if (OnTransferProgress != null)
			{
				OnTransferProgress(src, dst, transferredBytes, totalBytes, msg);
			}
		}

		#endregion
	}
}
