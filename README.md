Terminal-based initial setup program
====================================

Designed for headless servers.  The idea of this program
is that if:
 
 - The root password is locked
 - There are no regular local non-root accounts
 - There is no /root/.ssh/authorized_keys (not implemented yet)

Then we assume the system is unconfigured (e.g. it's a precanned
virtual machine disk) and ask for a new root password on the physical
console.

TODO
----

 - Optionally ask to create a regular user
 - License agreement
