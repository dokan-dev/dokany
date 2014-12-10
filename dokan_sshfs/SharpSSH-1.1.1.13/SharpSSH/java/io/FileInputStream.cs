using System;
using IO = System.IO;

namespace Tamir.SharpSsh.java.io
{
	/// <summary>
	/// Summary description for FileInputStream.
	/// </summary>
	public class FileInputStream : InputStream
	{
		IO.FileStream fs;
		public FileInputStream(string file)
		{
			fs = IO.File.OpenRead(file);
		}

		public FileInputStream(File file):this(file.info.Name)
		{
		}

		public override void Close()
		{
			fs.Close();
		}


		public override int Read(byte[] buffer, int offset, int count)
		{
			return fs.Read(buffer, offset, count);
		}

		public override bool CanSeek
		{
			get
			{
				return fs.CanSeek;
			}
		}

		public override long Seek(long offset, IO.SeekOrigin origin)
		{
			return fs.Seek(offset, origin);
		}
	}
}
