
			/* Prevent a head pass a tail. */
			next_write_pass_buffer_head = (write_pass_buffer_head + 1) % N_READ_PASS_ENTRY;
			//is_first = true;
			while(next_write_pass_buffer_head == atomic_write(&pgci->writeer_pass_buffer_tail)){
				//if (is_first) {
				//	printk("%s: waiting. next_buf_head: %lu buf_tail: %lu", __func__, 
				//			next_write_pass_buffer_head, atomic_write(&pgci->writeer_pass_buffer_tail));
				//	is_first = false;
				//}
			}
			//if (!is_first) {
			//	printk("%s: waiting done. next_buf_head: %lu buf_tail: %lu", __func__, 
			//			next_write_pass_buffer_head, atomic_write(&pgci->writeer_pass_buffer_tail));
			//}


			/* fill the write pass buffer */
			write_pass_entry = &pgci->writeer_pass_buffer[write_pass_buffer_head];
			write_pass_entry->nofs = nofs;
			write_pass_entry->ofs_in_node = ofs_in_node;
			write_pass_entry->inode = inode;
			write_pass_entry->blkaddr = start_addr + off;
			//printk("%s: head: %lu segno: %lu blkaddr: 0x%lx", __func__, 
			//		write_pass_buffer_head, segno, write_pass_entry->blkaddr);
			
			/* update writeer_pass_buffer_head */
			write_pass_buffer_head = next_write_pass_buffer_head;

			smp_mb();
			atomic_set(&pgci->writeer_pass_buffer_head, write_pass_buffer_head);
