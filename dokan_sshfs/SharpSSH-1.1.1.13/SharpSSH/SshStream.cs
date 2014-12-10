using System;
using System.IO;
using System.Collections;
using Tamir.SharpSsh.jsch;
using Tamir.Streams;
using System.Text;
using System.Text.RegularExpressions;

/* 
 * SshStream.cs
 * 
 * THIS SOURCE CODE IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY
 * KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A PARTICULAR
 * PURPOSE.
 * 
 * Copyright (C) 2005 Tamir Gal, tamirgal@myrealbox.com.
 */

namespace Tamir.SharpSsh
{
	/// <summary>
	/// A Stream based SSH class
	/// </summary>
	public class SshStream : Stream
	{
		private Stream m_in;
		private Stream m_out;
		private ChannelShell m_channel;
		private string m_host;
		private Regex m_prompt;
		private string m_escapeCharPattern;
		private bool m_removeTerminalChars = false;
		private Session m_session;

		/// <summary>
		/// Constructs a new SSH stream.
		/// </summary>
		/// <param name="host">The hostname or IP address of the remote SSH machine</param>
		/// <param name="username">The name of the user connecting to the remote machine</param>
		/// <param name="password">The password of the user connecting to the remote machine</param>
		public SshStream(string host, string username, string password)
		{
			this.m_host = host;
			JSch jsch=new JSch();
			m_session=jsch.getSession(username, host, 22);
			m_session.setPassword( password );
		
			Hashtable config=new Hashtable();
			config.Add("StrictHostKeyChecking", "no");
			m_session.setConfig(config);
		
			m_session.connect();
			m_channel=(ChannelShell)m_session.openChannel("shell");

			m_in	= m_channel.getInputStream();
			m_out	= m_channel.getOutputStream();

			m_channel.connect();
			m_channel.setPtySize(80, 132, 1024, 768);

			Prompt = "\n";
			m_escapeCharPattern = "\\[[0-9;?]*[^0-9;]";
		}

		/// <summary>
		/// Reads a sequence of bytes from the current stream and advances the position within the stream by the number of bytes read.
		/// </summary>
		/// <param name="buffer">An array of bytes. When this method returns, the buffer contains the specified byte array with the values between offset and (offset + count- 1) replaced by the bytes read from the current source.</param>
		/// <param name="offset">The zero-based byte offset in buffer at which to begin storing the data read from the current stream. </param>
		/// <param name="count">The maximum number of bytes to be read from the current stream. </param>
		/// <returns>The total number of bytes read into the buffer. This can be less than the number of bytes requested if that many bytes are not currently available, or zero (0) if the end of the stream has been reached.</returns>
		public override int Read(byte[] buffer, int offset, int count)
		{
			return m_in.Read(buffer, offset, count);
		}

		/// <summary>
		/// Reads a sequence of bytes from the current stream and advances the position within the stream by the number of bytes read.
		/// </summary>
		/// <param name="buffer">An array of bytes. When this method returns, the buffer contains the specified byte array with the values between offset and (offset + count- 1) replaced by the bytes read from the current source.</param>
		/// <returns>The total number of bytes read into the buffer. This can be less than the number of bytes requested if that many bytes are not currently available, or zero (0) if the end of the stream has been reached.</returns>
		public virtual int Read(byte[] buffer)
		{
			return Read(buffer, 0, buffer.Length);
		}

		/// <summary>
		/// Reads a byte from the stream and advances the position within the stream by one byte, or returns -1 if at the end of the stream.
		/// </summary>
		/// <returns>The unsigned byte cast to an Int32, or -1 if at the end of the stream.</returns>
		public override int ReadByte()
		{
			return m_in.ReadByte();
		}
		
		/// <summary>
		/// Writes a byte to the current position in the stream and advances the position within the stream by one byte.
		/// </summary>
		/// <param name="value">The byte to write to the stream. </param>
		public override void WriteByte(byte value)
		{
			m_out.WriteByte(value);
		}

		/// <summary>
		/// Writes a sequence of bytes to the current stream and advances the current position within this stream by the number of bytes written.
		/// </summary>
		/// <param name="buffer">An array of bytes. This method copies count bytes from buffer to the current stream. </param>
		/// <param name="offset">The zero-based byte offset in buffer at which to begin copying bytes to the current stream.</param>
		/// <param name="count">The number of bytes to be written to the current stream. </param>
		public override void Write(byte[] buffer, int offset, int count)
		{
			m_out.Write(buffer, offset, count);
		}

		/// <summary>
		/// Writes a sequence of bytes to the current stream and advances the current position within this stream by the number of bytes written.
		/// </summary>
		/// <param name="buffer">An array of bytes. This method copies count bytes from buffer to the current stream. </param>
		public virtual void Write(byte[] buffer)
		{
			Write(buffer, 0, buffer.Length);
		}

		/// <summary>
		/// Writes a String into the SSH channel. This method appends an 'end of line' character to the input string.
		/// </summary>
		/// <param name="data">The String to write to the SSH channel.</param>
		public void Write(String data)
		{
			data += "\r";
			Write( Encoding.Default.GetBytes( data ) );
			Flush();
		}

		/// <summary>
		/// Closes the current stream and releases any resources (such as sockets and file handles) associated with the current stream.
		/// </summary>
		public override void Close()
		{
			try
			{
				base.Close ();
				m_in.Close();
				m_out.Close();
				m_channel.close();
				m_channel.disconnect();
				m_session.disconnect();
			}
			catch{}
		}

		/// <summary>
		/// Gets a value indicating whether the current stream supports reading.
		/// </summary>
		public override bool CanRead
		{
			get
			{
				return m_in.CanRead;
			}
		}

		/// <summary>
		/// Gets a value indicating whether the current stream supports writing.
		/// </summary>
		public override bool CanWrite
		{
			get
			{
				return m_out.CanWrite;
			}
		}

		/// <summary>
		/// Gets a value indicating whether the current stream supports seeking. This stream cannot seek, and will always return false.
		/// </summary>
		public override bool CanSeek
		{
			get
			{
				return false;
			}
		}

		/// <summary>
		/// Clears all buffers for this stream and causes any buffered data to be written to the underlying device.
		/// </summary>
		public override void Flush()
		{
			m_out.Flush();
		}

		/// <summary>
		/// Gets the length in bytes of the stream.
		/// </summary>
		public override long Length
		{
			get
			{
				return 0;
			}
		}

		/// <summary>
		/// Gets or sets the position within the current stream. This Stream cannot seek. This property has no effect on the Stream and will always return 0.
		/// </summary>
		public override long Position
		{
			get
			{
				return 0;
			}
			set
			{
			}
		}

		/// <summary>
		/// This method has no effect on the Stream.
		/// </summary>
		public override void SetLength(long value)
		{
		}

		/// <summary>
		/// This method has no effect on the Stream.
		/// </summary>
		public override long Seek(long offset, SeekOrigin origin)
		{
			return 0;
		}

		/// <summary>
		/// A regular expression pattern string to match when reading the resonse using the ReadResponse() method. The default prompt value is "\n" which makes the ReadRespons() method return one line of response.
		/// </summary>
		public string Prompt
		{
			get{return m_prompt.ToString();}
			set{m_prompt = new Regex(value, RegexOptions.Singleline);}
		}

		/// <summary>
		/// Gets or sets a value indicating wether this Steam sould remove the terminal emulation's escape sequence characters from the response String.
		/// </summary>
		public bool RemoveTerminalEmulationCharacters
		{
			get{return m_removeTerminalChars;}
			set{m_removeTerminalChars=value;}
		}

		/// <summary>
		/// Gets the Cipher algorithm name used in this SSH connection.
		/// </summary>
		public string Cipher
		{
			get{return m_session.getCipher();}
		}

		/// <summary>
		/// Gets the MAC algorithm name used in this SSH connection.
		/// </summary>
		public string Mac
		{
			get{return m_session.getMac();}
		}

		/// <summary>
		/// Gets the server SSH version string.
		/// </summary>
		public string ServerVersion
		{
			get{return m_session.getServerVersion();}
		}

		/// <summary>
		/// Gets the client SSH version string.
		/// </summary>
		public string ClientVersion
		{
			get{return m_session.getClientVersion();}
		}

		/// <summary>
		/// Reads a response string from the SSH channel. This method will block until the pattern in the 'Prompt' property is matched in the response.
		/// </summary>
		/// <returns>A response string from the SSH server.</returns>
		public string ReadResponse()
		{
			int readCount;
			StringBuilder resp = new StringBuilder();
			byte[] buff = new byte[1024];
			Match match;

			do 
			{
				readCount = this.Read(buff);
				resp.Append( System.Text.Encoding.Default.GetString( buff), 0, readCount);
				string s = resp.ToString();
				match = m_prompt.Match( s );
			}while(!match.Success);

			return HandleTerminalChars( resp.ToString() );
		}

		/// <summary>
		/// Removes escape sequence characters from the input string.
		/// </summary>
		/// <param name="str"></param>
		/// <returns></returns>
		private string HandleTerminalChars(string str)
		{
			if(RemoveTerminalEmulationCharacters)
			{
				str = str.Replace("(B)0", "");
				str = Regex.Replace(str, m_escapeCharPattern, "");
				str = str.Replace(((char)15).ToString(), "");
				str = Regex.Replace(str, ((char)27).ToString()+"=*", "");
				//str = Regex.Replace(str, "\\s*\r\n", "\r\n");
			}
			return str;
		}
	}
}
