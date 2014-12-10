using System;
using System.Drawing;
using System.Collections;
using System.ComponentModel;
using System.Windows.Forms;

namespace Tamir.SharpSsh.jsch.examples
{
	/// <summary>
	/// Summary description for ProgressForm.
	/// </summary>
	public class ProgressForm : System.Windows.Forms.Form
	{
		private System.Windows.Forms.ProgressBar progressBar1;
		private System.Windows.Forms.Button btnCancel;
		private System.Windows.Forms.Label lblProgress;
		/// <summary>
		/// Required designer variable.
		/// </summary>
		private System.ComponentModel.Container components = null;

		public ProgressForm()
		{
			//
			// Required for Windows Form Designer support
			//
			InitializeComponent();
			progressBar1.Minimum = 0;
			progressBar1.Maximum = 100;
		}

		/// <summary>
		/// Clean up any resources being used.
		/// </summary>
		protected override void Dispose( bool disposing )
		{
			if( disposing )
			{
				if(components != null)
				{
					components.Dispose();
				}
			}
			base.Dispose( disposing );
		}

		public void Update(int progress, string Text)
		{
			this.BeginInvoke( new invoke(UpdateProgress), new object[]{progress, Text} );
		}

		public void Finish()
		{
			this.Close();
		}

		private void UpdateProgress(int progress, string text)
		{
			lblProgress.Text=text;
			progressBar1.Value=progress;
		}

		#region Windows Form Designer generated code
		/// <summary>
		/// Required method for Designer support - do not modify
		/// the contents of this method with the code editor.
		/// </summary>
		private void InitializeComponent()
		{
			this.progressBar1 = new System.Windows.Forms.ProgressBar();
			this.btnCancel = new System.Windows.Forms.Button();
			this.lblProgress = new System.Windows.Forms.Label();
			this.SuspendLayout();
			// 
			// progressBar1
			// 
			this.progressBar1.Location = new System.Drawing.Point(48, 32);
			this.progressBar1.Name = "progressBar1";
			this.progressBar1.Size = new System.Drawing.Size(264, 23);
			this.progressBar1.TabIndex = 0;
			// 
			// btnCancel
			// 
			this.btnCancel.Location = new System.Drawing.Point(144, 64);
			this.btnCancel.Name = "btnCancel";
			this.btnCancel.TabIndex = 1;
			this.btnCancel.Text = "Cancel";
			// 
			// lblProgress
			// 
			this.lblProgress.Location = new System.Drawing.Point(48, 8);
			this.lblProgress.Name = "lblProgress";
			this.lblProgress.Size = new System.Drawing.Size(264, 23);
			this.lblProgress.TabIndex = 2;
			this.lblProgress.TextAlign = System.Drawing.ContentAlignment.BottomLeft;
			// 
			// ProgressForm
			// 
			this.AcceptButton = this.btnCancel;
			this.AutoScaleBaseSize = new System.Drawing.Size(5, 13);
			this.ClientSize = new System.Drawing.Size(368, 94);
			this.ControlBox = false;
			this.Controls.Add(this.lblProgress);
			this.Controls.Add(this.btnCancel);
			this.Controls.Add(this.progressBar1);
			this.Name = "ProgressForm";
			this.Text = "ProgressForm";
			this.ResumeLayout(false);

		}
		#endregion
	}

	public delegate void invoke(int progress, string Text);
}
