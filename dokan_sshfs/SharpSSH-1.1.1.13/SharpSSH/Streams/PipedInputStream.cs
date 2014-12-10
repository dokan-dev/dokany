using System;
using System.IO;
using System.Runtime.CompilerServices;
using System.Threading;

namespace Tamir.Streams
{
/*
 * @(#)PipedInputStream.java	1.35 03/12/19
 *
 * Copyright 2004 Sun Microsystems, Inc. All rights reserved.
 * SUN PROPRIETARY/CONFIDENTIAL. Use is subject to license terms.
 */

/**
 * A piped input stream should be connected
 * to a piped output stream; the piped  input
 * stream then provides whatever data bytes
 * are written to the piped output  stream.
 * Typically, data is read from a <code>PipedInputStream</code>
 * object by one thread  and data is written
 * to the corresponding <code>PipedOutputStream</code>
 * by some  other thread. Attempting to use
 * both objects from a single thread is not
 * recommended, as it may deadlock the thread.
 * The piped input stream contains a buffer,
 * decoupling read operations from write operations,
 * within limits.
 *
 * @author  James Gosling
 * @version 1.35, 12/19/03
 * @see     java.io.PipedOutputStream
 * @since   JDK1.0
 */
	public class PipedInputStream : Tamir.SharpSsh.java.io.InputStream
	{
		internal bool closedByWriter = false;
		internal volatile bool closedByReader = false;
		internal bool connected = false;

		/* REMIND: identification of the read and write sides needs to be
		   more sophisticated.  Either using thread groups (but what about
		   pipes within a thread?) or using finalization (but it may be a
		   long time until the next GC). */
		internal Thread readSide;
		internal Thread writeSide;

		/**
		 * The size of the pipe's circular input buffer.
		 * @since   JDK1.1
		 */
		internal const int PIPE_SIZE = 1024;

		/**
		 * The circular buffer into which incoming data is placed.
		 * @since   JDK1.1
		 */
		internal byte[] buffer = new byte[PIPE_SIZE];

		/**
		 * The index of the position in the circular buffer at which the
		 * next byte of data will be stored when received from the connected
		 * piped output stream. <code>in&lt;0</code> implies the buffer is empty,
		 * <code>in==out</code> implies the buffer is full
		 * @since   JDK1.1
		 */
		internal int m_in = -1;

		/**
		 * The index of the position in the circular buffer at which the next
		 * byte of data will be read by this piped input stream.
		 * @since   JDK1.1
		 */
		internal int m_out = 0;

		/**
		 * Creates a <code>PipedInputStream</code> so
		 * that it is connected to the piped output
		 * stream <code>src</code>. Data bytes written
		 * to <code>src</code> will then be  available
		 * as input from this stream.
		 *
		 * @param      src   the stream to connect to.
		 * @exception  IOException  if an I/O error occurs.
		 */
		public PipedInputStream(PipedOutputStream src) 
		{
			connect(src);
		}

		/**
		 * Creates a <code>PipedInputStream</code> so
		 * that it is not  yet connected. It must be
		 * connected to a <code>PipedOutputStream</code>
		 * before being used.
		 *
		 * @see     java.io.PipedInputStream#connect(java.io.PipedOutputStream)
		 * @see     java.io.PipedOutputStream#connect(java.io.PipedInputStream)
		 */
		public PipedInputStream() 
		{
			int i = 0;
		}

		/**
		 * Causes this piped input stream to be connected
		 * to the piped  output stream <code>src</code>.
		 * If this object is already connected to some
		 * other piped output  stream, an <code>IOException</code>
		 * is thrown.
		 * <p>
		 * If <code>src</code> is an
		 * unconnected piped output stream and <code>snk</code>
		 * is an unconnected piped input stream, they
		 * may be connected by either the call:
		 * <p>
		 * <pre><code>snk.connect(src)</code> </pre>
		 * <p>
		 * or the call:
		 * <p>
		 * <pre><code>src.connect(snk)</code> </pre>
		 * <p>
		 * The two
		 * calls have the same effect.
		 *
		 * @param      src   The piped output stream to connect to.
		 * @exception  IOException  if an I/O error occurs.
		 */
		public virtual void connect(PipedOutputStream src)
		{
			src.connect(this);
		}

		/**
		 * Receives a byte of data.  This method will block if no input is
		 * available.
		 * @param b the byte being received
		 * @exception IOException If the pipe is broken.
		 * @since     JDK1.1
		 */
		[MethodImpl(MethodImplOptions.Synchronized)]
		internal void receive(int b) 
		{
			checkStateForReceive();
			writeSide = Thread.CurrentThread;
			if (m_in == m_out)
				awaitSpace();
			if (m_in < 0) 
			{
				m_in = 0;
				m_out = 0;
			}
			buffer[m_in++] = (byte)(b & 0xFF);
			if (m_in >= buffer.Length) 
			{
				m_in = 0;
			}
		}

		/**
		 * Receives data into an array of bytes.  This method will
		 * block until some input is available.
		 * @param b the buffer into which the data is received
		 * @param off the start offset of the data
		 * @param len the maximum number of bytes received
		 * @exception IOException If an I/O error has occurred.
		 */
		[MethodImpl(MethodImplOptions.Synchronized)]
		internal void receive(byte[] b, int off, int len) 
		{
			checkStateForReceive();
			writeSide = Thread.CurrentThread;
			int bytesToTransfer = len;
			while (bytesToTransfer > 0) 
			{
				if (m_in == m_out)
					awaitSpace();
				int nextTransferAmount = 0;
				if (m_out < m_in) 
				{
					nextTransferAmount = buffer.Length - m_in;
				} 
				else if (m_in < m_out) 
				{
					if (m_in == -1) 
					{
						m_in = m_out = 0;
						nextTransferAmount = buffer.Length - m_in;
					} 
					else 
					{
						nextTransferAmount = m_out - m_in;
					}
				}
				if (nextTransferAmount > bytesToTransfer)
					nextTransferAmount = bytesToTransfer;
				assert(nextTransferAmount > 0);
				Array.Copy(b, off, buffer, m_in, nextTransferAmount);
				bytesToTransfer -= nextTransferAmount;
				off += nextTransferAmount;
				m_in += nextTransferAmount;
				if (m_in >= buffer.Length) 
				{
					m_in = 0;
				}
			}
		}

		private void checkStateForReceive()
		{
			if (!connected) 
			{
				throw new IOException("Pipe not connected");
			} 
			else if (closedByWriter || closedByReader) 
			{
				throw new IOException("Pipe closed");
			} 
			else if (readSide != null && !readSide.IsAlive) 
			{
				throw new IOException("Read end dead");
			}
		}

		private void awaitSpace()  
		{
			while (m_in == m_out) 
			{
				if ((readSide != null) && !readSide.IsAlive) 
				{
					throw new IOException("Pipe broken");
				}
				/* full: kick any waiting readers */
				//java: notifyAll();
				Monitor.PulseAll(this);
				try 
				{
					//java: wait(1000);
					Monitor.Wait(this, 1000);
				} 
				catch (ThreadInterruptedException  ex) 
				{
					throw ex;
				}
			}
		}

		/**
		 * Notifies all waiting threads that the last byte of data has been
		 * received.
		 */
		[MethodImpl(MethodImplOptions.Synchronized)]
		internal void receivedLast() 
		{
			closedByWriter = true;
			//notifyAll();
			Monitor.PulseAll(this);
		}

		/**
		 * Reads the next byte of data from this piped input stream. The
		 * value byte is returned as an <code>int</code> in the range
		 * <code>0</code> to <code>255</code>. If no byte is available
		 * because the end of the stream has been reached, the value
		 * <code>-1</code> is returned. This method blocks until input data
		 * is available, the end of the stream is detected, or an exception
		 * is thrown.
		 * If a thread was providing data bytes
		 * to the connected piped output stream, but
		 * the  thread is no longer alive, then an
		 * <code>IOException</code> is thrown.
		 *
		 * @return     the next byte of data, or <code>-1</code> if the end of the
		 *             stream is reached.
		 * @exception  IOException  if the pipe is broken.
		 */
		[MethodImpl(MethodImplOptions.Synchronized)]
		public virtual  int read()  
		{
			if (!connected) 
			{
				throw new IOException("Pipe not connected");
			} 
			else if (closedByReader) 
			{
				throw new IOException("Pipe closed");
			} 
			else if (writeSide != null && !writeSide.IsAlive
				&& !closedByWriter && (m_in < 0)) 
			{
				throw new IOException("Write end dead");
			}

			readSide = Thread.CurrentThread;
			int trials = 2;
			while (m_in < 0) 
			{
				if (closedByWriter) 
				{
					/* closed by writer, return EOF */
					return -1;
				}
				if ((writeSide != null) && (!writeSide.IsAlive) && (--trials < 0)) 
				{
					throw new IOException("Pipe broken");
				}
				/* might be a writer waiting */
				Monitor.PulseAll(this);
				try 
				{
					Monitor.Wait(this, 1000);
				} 
				catch (ThreadInterruptedException ex) 
				{
					throw ex;
				}
			}
			int ret = buffer[m_out++] & 0xFF;
			if (m_out >= buffer.Length) 
			{
				m_out = 0;
			}
			if (m_in == m_out) 
			{
				/* now empty */
				m_in = -1;
			}
			return ret;
		}

		/**
		 * Reads up to <code>len</code> bytes of data from this piped input
		 * stream into an array of bytes. Less than <code>len</code> bytes
		 * will be read if the end of the data stream is reached. This method
		 * blocks until at least one byte of input is available.
		 * If a thread was providing data bytes
		 * to the connected piped output stream, but
		 * the  thread is no longer alive, then an
		 * <code>IOException</code> is thrown.
		 *
		 * @param      b     the buffer into which the data is read.
		 * @param      off   the start offset of the data.
		 * @param      len   the maximum number of bytes read.
		 * @return     the total number of bytes read into the buffer, or
		 *             <code>-1</code> if there is no more data because the end of
		 *             the stream has been reached.
		 * @exception  IOException  if an I/O error occurs.
		 */
		[MethodImpl(MethodImplOptions.Synchronized)]
		public override int read(byte[] b, int off, int len)
		{
			if (b == null) 
			{
				throw new NullReferenceException();
			} 
			else if ((off < 0) || (off > b.Length) || (len < 0) ||
				((off + len) > b.Length) || ((off + len) < 0)) 
			{
				throw new IndexOutOfRangeException();
			} 
			else if (len == 0) 
			{
				return 0;
			}

			/* possibly wait on the first character */
			int c = read();
			if (c < 0) 
			{
				return -1;
			}
			b[off] = (byte) c;
			int rlen = 1;
			while ((m_in >= 0) && (--len > 0)) 
			{
				b[off + rlen] = buffer[m_out++];
				rlen++;
				if (m_out >= buffer.Length) 
				{
					m_out = 0;
				}
				if (m_in == m_out) 
				{
					/* now empty */
					m_in = -1;
				}
			}
			return rlen;
		}

		/**
		 * Returns the number of bytes that can be read from this input
		 * stream without blocking. This method overrides the <code>available</code>
		 * method of the parent class.
		 *
		 * @return     the number of bytes that can be read from this input stream
		 *             without blocking.
		 * @exception  IOException  if an I/O error occurs.
		 * @since   JDK1.0.2
		 */
		[MethodImpl(MethodImplOptions.Synchronized)]
		public virtual int available() 
		{
			if(m_in < 0)
				return 0;
			else if(m_in == m_out)
				return buffer.Length;
			else if (m_in > m_out)
				return m_in - m_out;
			else
				return m_in + buffer.Length - m_out;
		}

		/**
		 * Closes this piped input stream and releases any system resources
		 * associated with the stream.
		 *
		 * @exception  IOException  if an I/O error occurs.
		 */
		public override void close()  
		{
			closedByReader = true;
			lock (this) 
			{
				m_in = -1;
			}
		}

		private void assert(bool exp)
		{
			if (!exp)
				throw new Exception("Assertion failed!");
		}

		///////////////////////////////////////
		

		public override int Read(byte[] buffer, int offset, int count)
		{
			return this.read(buffer, offset, count);
		}

		public override int ReadByte()
		{
			return this.read();
		}
		
		public override void WriteByte(byte value)
		{
		}

		public override void Write(byte[] buffer, int offset, int count)
		{
		}
		public override void Close()
		{
			base.Close ();
			this.close();
		}
		public override bool CanRead
		{
			get
			{
				return true;
			}
		}
		public override bool CanWrite
		{
			get
			{
				return false;
			}
		}
		public override bool CanSeek
		{
			get
			{
				return false;
			}
		}
		public override void Flush()
		{
			
		}
		public override long Length
		{
			get
			{
				if(m_in > m_out)
					return (m_in - m_out);
				else
				{
					return (buffer.Length -m_out+m_in);
				}
			}
		}
		public override long Position
		{
			get
			{
				return m_out;
			}
			set
			{
				throw new IOException("Setting the position of this stream is not supported");
			}
		}
		public override void SetLength(long value)
		{
			throw new IOException("Setting the length of this stream is not supported");
		}
		public override long Seek(long offset, SeekOrigin origin)
		{
			return 0;
		}
	}
}
