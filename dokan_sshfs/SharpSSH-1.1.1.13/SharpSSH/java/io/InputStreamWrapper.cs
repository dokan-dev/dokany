using Tamir.SharpSsh.java.io;

namespace Tamir.Streams
{
	/// <summary>
	/// Summary description for InputStreamWrapper.
	/// </summary>
	public class InputStreamWrapper : InputStream
	{
		System.IO.Stream s;
		public InputStreamWrapper(System.IO.Stream s)
		{
			this.s = s;
		}

		public override int Read(byte[] buffer, int offset, int count)
		{
			return s.Read(buffer, offset, count);
		}
	}
}
