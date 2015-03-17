;; Maildirwatch -- watch a maildir for new messages
;; Copyright (C) 2014 Giuseppe Scrivano <gscrivano@gnu.org>

;; Maildirwatch is free software; you can redistribute it and/or modify
;; it under the terms of the GNU General Public License as published by
;; the Free Software Foundation; either version 3 of the License, or
;; (at your option) any later version.

;; GNU Wget is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU General Public License for more details.

;; You should have received a copy of the GNU General Public License
;; along with Wget.  If not, see <http://www.gnu.org/licenses/>.

(setq-default maildirwatch-path "/usr/bin/maildirwatch")

(defun maildirwatch-desc (file)
  (with-current-buffer (find-file-literally file)
    (unwind-protect
        (let ((start (re-search-forward "Subject:+ " nil t)))
          (if start
              (progn (end-of-line)
                     (buffer-substring start (point)))
            file))
      (kill-buffer (current-buffer)))))

(defun maildirwatch-filter (proc file)
  (message (concat "new message "
                   (maildirwatch-desc
                    (replace-regexp-in-string "\n$" "" file)))))

(defun maildirwatch-watch (maildirs)
  (interactive)
  (let* ((mdirs-path (mapcar 'expand-file-name maildirwatch-mdirs))
        (proc (apply 'start-process "maildirwatch" nil
                     maildirwatch-path mdirs-path)))
    (set-process-filter proc 'maildirwatch-filter)
    proc))
