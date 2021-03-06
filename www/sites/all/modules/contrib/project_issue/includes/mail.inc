<?php

/**
 * @file
 * Project issue tracking email notifications system.
 */

/**
 * preg_replace_callback() callback used for capturing URLs and transforming links
 * into footer links.
 */
function _project_issue_mail_url_callback($match = FALSE) {
  static $urls = array();
  if (!empty($match) && is_array($match)) {
    $url = $match[2];

    // If $url was an internal link, we need to strip the base_path
    // off before passing it to url(), to avoid doubling up on the base_path.
    // @todo Check that this actually works when using language specific url rewriting.
    $base_path = base_path();
    $base_length = strlen($base_path);
    if (substr($url, 0, $base_length) == $base_path) {
      $url = substr($url, $base_length);
    }

    $urls[] = url($url, array('absolute' => TRUE));
    return $match[3] . ' [' . count($urls) . ']';
  }
  return $urls;
}

/**
 * Add a section to the email body.
 * Note: URL link numbers are not reset between calls to this function.
 */
function project_issue_mail_output(&$body, $html = 1, $format = FILTER_FORMAT_DEFAULT) {
  static $i = 0;

  if ($html) {
    $body = check_markup($body, $format, FALSE);

    // Convert inline links into footer links.
    //$pattern = '@<a +([^ >]+ )*?href *= *"([^>"]+?)"[^>]*>([^<]+?)</a>@i';
    $pattern = '@<a[^>]*\shref\s*=\s*([\'"])([^>]+?)\1[^>]*>(.+?)</a>@is';
    $body = preg_replace_callback($pattern, '_project_issue_mail_url_callback', $body);
    $urls = _project_issue_mail_url_callback();

    if (count($urls)) {
      $body .= "\n";
      for ($max = count($urls); $i < $max; $i++) {
        $body .= '[' . ($i + 1) . '] ' . $urls[$i] . "\n";
      }
    }

    $body = preg_replace('!</?blockquote>!i', '"', $body);
    $body = preg_replace('!</?(em|i)>!i', '/', $body);
    $body = preg_replace('!</?(b|strong)>!i', '*', $body);
    $body = preg_replace("@<br />(?!\n)@i", "\n", $body);
    $body = preg_replace("@</p>(?!\n\n)@i", "\n\n", $body);
    $body = preg_replace("@<li>@i", "* ", $body);
    $body = preg_replace("@</li>\n?@i", "\n", $body);
    $body = strip_tags($body);
    $body = decode_entities($body);
    $body = wordwrap($body, 72);
  }
  else {
    $body = decode_entities($body);
  }
}

/**
 * Send mail notifications.
 */
function project_issue_mail_notify($nid) {
  global $user;

  if (defined('PROJECT_NOMAIL')) {
    return;
  }

  $node = node_load($nid, NULL, TRUE);

  // We don't want any notifications for unpublished nodes.
  if (empty($node) || !$node->status) {
    return;
  }

  $project = node_load($node->field_project[LANGUAGE_NONE][0]['target_id']);

  // Store a copy of the issue, so we can load the original issue values
  // below.
  $issue = drupal_clone($node);

  // Load in the original issue data here, since we want a running
  // reverse history.
  // TODO: Where is original_issue_data stored now?
  $original_issue_data = unserialize($node->project_issue['original_issue_data']);
  // TODO: This function doesn't seem to exist in 7.x.
  $fields = project_issue_field_labels('email');
  foreach ($fields as $field => $label) {
    if ($field != 'name' && $field != 'updator') {
      $issue->original_issue_metadata->$field = $original_issue_data->$field;
    }
  }

  // Record users that are connected to this issue.
  $uids = array();
  if (!empty($node->uid)) {
    $uids[$node->uid] = $node->uid;
  }
  $assigned_uid = $node->field_issue_assigned[LANGUAGE_NONE][0]['target_id'];
  if (!empty($assigned_uid)) {
    $uids[$assigned_uid] = $assigned_uid;
  }

  // Create complete history of the bug report.
  $history = array($issue);
  $result = db_query('SELECT u.name, c.cid, c.nid, c.subject, c.comment, c.uid, c.format, pic.* FROM {project_issue_comments} pic INNER JOIN {comments} c ON c.cid = pic.cid INNER JOIN {users} u ON u.uid = c.uid WHERE c.nid = %d AND c.status = %d ORDER BY pic.timestamp', $node->nid, COMMENT_PUBLISHED);

  while ($comment = db_fetch_object($result)) {
    $comment->comment = db_decode_blob($comment->comment);
    $comment->files = comment_upload_load_files($comment->cid);
    $history[] = $comment;
    // Record users that are connected to this issue.
    if ($comment->uid) {
      $uids[$comment->uid] = $comment->uid;
    }
  }

  // Check whether Flag module integration is enabled for e-mail notifications.
  $follow_flag = project_issue_get_follow_flag($node->type);

  // If flag integration is present, the list of users for 'own' issues will
  // be those that flagged the issue.
  if (!empty($follow_flag)) {
    // Retrieve all users who flagged the issue.
    $flag_contents = flag_get_content_flags('node', $node->nid, $follow_flag);
    $own_issues_uids = array_keys($flag_contents);
  }
  // Without flag integration, 'own issues' are all users that posted to the
  // issue.
  else {
    $own_issues_uids = array_keys($uids);
  }


  // Initialize the recipient list.  After compilation, the list will include
  // users that are:
  //  - subscribed to all issues of the project
  //  - subscribed to "own" issues of the project
  //  - subscribed to all issues by default (global default)
  //  - subscribed to "own" issues by default (global default)

  // Note that "own" changes meaning depending on if flag.module is enabled
  // and configured for following issues.
  $recipients = array();

  // In both queries, the first placeholder is always the node ID of the
  // project that the current issue belongs to, since we need to restrict to
  // that project for the per-project settings, and LEFT JOIN on that project
  // to catch all the users with a default notification setting (everyone who
  // doesn't have a specific preference for that project).
  $args = array($node->project_issue['pid']);

  // Build up filters for different issue notification levels.
  $filter_array = array();
  // 'All' level.
  $filter_array[] = "(pi.level = " . PROJECT_ISSUE_NOTIFICATION_ALL . ")";
  // 'Own' level using the list of users attached to the issue.
  if (!empty($own_issues_uids)) {
    $placeholders = implode(',', array_fill(0, count($own_issues_uids), '%d'));
    $filter_array[] = "(pi.level = " . PROJECT_ISSUE_NOTIFICATION_OWN . " AND u.uid IN ($placeholders))";
    $args = array_merge($args, $own_issues_uids);
  }
  $filter = implode(" OR ", $filter_array);

  // Pull users that want notifications at the project level.
  $project_notification_query = "SELECT pi.uid, u.name, u.mail
    FROM {project_issue_notification_project} pi
    INNER JOIN {users} u ON pi.uid = u.uid
    WHERE u.status = 1 AND pi.nid = %d AND (" . $filter . ")";
  $result = db_query($project_notification_query, $args);
  while ($account = db_fetch_object($result)) {
    $recipients[$account->uid] = $account;
  }

  // Pull users that want global notifications if they haven't defined a
  // per-project setting for this project.
  $default_notification_query = "SELECT pi.uid, u.name, u.mail
    FROM {project_issue_notification_global} pi
    LEFT JOIN {project_issue_notification_project} pinp ON pi.uid = pinp.uid AND pinp.nid = %d
    INNER JOIN {users} u ON pi.uid = u.uid
    WHERE u.status = 1 AND pinp.level IS NULL AND (" . $filter . ")";
  $result = db_query($default_notification_query, $args);
  while ($account = db_fetch_object($result)) {
    $recipients[$account->uid] = $account;
  }

  // To save workload, check here if either the anonymous role or the
  // authenticated role has the 'view uploaded files' permission, since
  // we only need to process each user's file access permission if this
  // is NOT the case.
  $check_file_perms = !db_result(db_query("SELECT COUNT(*) FROM {permission} WHERE perm LIKE '%view uploaded files%' AND rid IN (%d, %d)", DRUPAL_ANONYMOUS_RID, DRUPAL_AUTHENTICATED_RID));

  // We need to determine if node_access() checks are necessary.  The
  // check will be needed if any of the following is true:
  //   1. The node is not published.
  //   2. There is at least on node access control module enabled.
  //   3. Both the anonymous and authenticated user do not have
  //      the 'access project issues' permission.
  $allowed_roles = user_roles(FALSE, 'access project issues');
  if (isset($allowed_roles[DRUPAL_ANONYMOUS_RID]) || isset($allowed_roles[DRUPAL_AUTHENTICATED_RID])) {
    $anon_auth_access = TRUE;
  }
  $grants = module_implements('node_grants');
  $check_node_access = $node->status != 1 || empty($anon_auth_access) || !empty($grants);

  $params['node']    = $node;
  $params['project'] = $project;
  $params['history'] = $history;

  $sender->name = t('!name (!site)', array('!name' => $user->name, '!site' => variable_get('site_name', 'Drupal')));
  // @todo: the %project token replacement here is broken and should be ported
  // to use machine_name.
  $sender->mail = strtr(variable_get('project_issue_reply_to_' . $node->type, variable_get('site_mail', ini_get('sendmail_from'))), array('%project' => $project->project['uri']));
  // The sender name is enclosed by double quotes below
  // to satisfy RFC2822 <http://www.faqs.org/rfcs/rfc2822.html>,
  // which requires double quotes when special characters (including
  // some punctuation) are used.  See example in Appendix A.1.2.
  $from = '"' . mime_header_encode($sender->name) . "\" <$sender->mail>";

  // Pre-load the e-mail notification preferences for all users in 1 query.
  $user_preferences = project_issue_notification_user_settings_load_multiple(array_keys($recipients));

  // Send notification to each connected user.
  foreach ($recipients as $recipient) {
    // To save work, only go through a user_load if we need it.
    if ($check_file_perms || $check_node_access) {
      $account = user_load(array('uid' => $recipient->uid));
      $language = user_preferred_language($account);
    }
    else {
      $language = language_default();
    }
    // Load the recipient's content preferences for issue notification e-mails.
    $recipient->project_issue_notification = $user_preferences[$recipient->uid];
    $can_access = $check_node_access ? node_access('view', $node, $account) : TRUE;
    if ($can_access) {
      $display_files = $check_file_perms ? user_access('view uploaded files', $account) : TRUE;

      $params['display_files'] = $display_files;
      $params['recipient'] = $recipient;
      drupal_mail('project_issue', 'project_issue_update_notification', $recipient->mail, $language, $params, $from);
    }
  }

  if (is_array($project->project_issue['mail_copy_filter']) && count(array_filter($project->project_issue['mail_copy_filter'])) && !$project->project_issue['mail_copy_filter'][$node->project_issue['category']]) {
    return;
  }

  if (is_array($project->project_issue['mail_copy_filter_state']) && count(array_filter($project->project_issue['mail_copy_filter_state'])) && !$project->project_issue['mail_copy_filter_state'][$node->project_issue['sid']]) {
    return;
  }

  if (!empty($project->project_issue['mail_copy'])) {
    $params['display_files'] = TRUE;
    $message['body'][] = $links;
    $message['body'][] = project_issue_mail_generate_followup_mail_body($node, $history, TRUE);
    drupal_mail('project_issue', 'project_issue_update_notification', $project->project_issue['mail_copy'], language_default(), $params, $from);
  }
}

/**
 * Implementation of hook_mail().
 */
function _project_issue_mail($key, &$message, $params) {
  global $base_url;

  switch ($key) {
    case "project_issue_update_notification":
      // There could be stale data in the cached node, so reset the cache.
      $node    = $params['node'];
      $project = $params['project'];
      $history = $params['history'];
      $fields  = project_issue_field_labels('email');

      $domain = preg_replace('|.+://([a-zA-Z0-9\._-]+).*|', '\1', $base_url);

      $message['headers'] += array(
        'Date' => date('r'),
        'X-Mailer' => 'Drupal Project module (http://drupal.org/project/project)',
        'List-Id' => "$project->title <" . $project->project['uri'] . "-issues-$domain>",
        'List-Archive' => '<' . url('project/issues/' . $project->project['uri'], array('absolute' => TRUE)) . '>',
        'List-Subscribe' => '<' . url('project/issues/subscribe-mail/' . $project->project['uri'], array('absolute' => TRUE)) . '>',
        'List-Unsubscribe' => '<' . url('project/issues/subscribe-mail/' . $project->project['uri'], array('absolute' => TRUE)) . '>',
      );

      // Comments exist, set headers accordingly.
      if (count($history) > 1) {
        foreach ($history as $comment) {
          // We need the most recent cid and the next most recent cid for the
          // message headers.  Instead of issuing another query, just keep track
          // of them here.
          $previous_cid = isset($cid) ? $cid : '';
          $cid = isset($comment->cid) ? $comment->cid : 0;
        }
        $message['headers']['Message-Id'] = "<type=project&nid=$node->nid&cid=$cid&host=@$domain>";
        $message['headers']['In-Reply-To'] = "<type=project&nid=$node->nid&host=@$domain>";
        $message['headers']['References'] = "<type=project&nid=$node->nid&host=@$domain> <type=project&nid=$node->nid&cid=$previous_cid&host=@$domain> <type=project&nid=$node->nid&revcount=1&host=@$domain>";
      }
      else {
        // Only original issue in this email.
        $message['headers']['Message-Id'] = "<type=project&nid=$node->nid&host=@$domain>";
      }

      project_issue_mail_output($node->title, 0);

      // Create link to related node
      $links = t('Issue status update for !link', array('!link' => "\n" . url("node/$node->nid", array('absolute' => TRUE)))) . "\n";
      $links .= t('Post a follow up: !link', array('!link' => "\n" . url("comment/reply/$node->nid", array('fragment' => 'comment-form', 'absolute' => TRUE)))) . "\n";
      $body = project_issue_mail_generate_followup_mail_body($node, $history, $params['display_files'], $params['recipient']);

      $preferences = $params['recipient']->project_issue_notification;

      // Construct the appropriate subject based on the user's preferences.
      $subject_tokens = array(
        '!short_name' => $project->project['uri'],
        '!category' => $node->project_issue['category'],
        '!title' => $node->title,
      );
      $subject = array();
      if (!empty($preferences['mail_subject_project'])) {
        $subject[] = '[!short_name]';
      }
      if (!empty($preferences['mail_subject_category'])) {
        $subject[] = '[!category]';
      }
      $subject[] = '!title';

      $subject = strtr(implode(' ', $subject), $subject_tokens);

      $message['subject'] = $subject;
      $message['body']['links'] = $links;
      $message['body']['body'] = $body;
      break;

    case 'project_issue_critical_summary':
      $project = $params['project'];
      $message['headers'] += array(
        'Date' => date('r'),
        'X-Mailer' => 'Drupal Project Issues module (http://drupal.org/project/project_issue)',
        'List-Id' => "$project->title <" . preg_replace('|.+://([a-zA-Z0-9\._-]+).*|', '\1', $base_url) . '-project-issues-digest>',
        'List-Archive' => '<' . url('project/issues', array('query' => array('priorities' => '1'), 'absolute' => TRUE)) . '>',
      );
      $message['subject'] = t('Release critical bugs for !date', array('!date' => date('F d, Y', time())));
      $message['body'][] = $params['body'];
      break;

    case 'project_issue_reminder':
      $sender->name = variable_get('site_name', '');
      $sender->mail = variable_get('site_mail', '');
      $message['headers'] += array(
        'Return-Path' => "<$sender->mail;>",
        'Date' => date('r'),
        'From' => "$sender->name <$sender->mail>",
        'X-Mailer' => 'Drupal Project Issues module (http://drupal.org/project/project_issue)',
        'List-Id' => "$sender->name <project-reminder-" . preg_replace('|.+://([a-zA-Z0-9\._-]+).*|', '\1', $base_url) . '>',
        'List-Archive' => '<' . url('project', array('absolute' => TRUE)) . '>',
      );
      $message['subject'] = t('Your submitted bugs for !date', array('!date' => date('F d, Y', time())));
      $message['body'][] = $params['body'];
      break;
  }
}

/**
 * Format the body of an issue followup email.
 *
 * @param $node
 *   The issue node.
 * @param $history
 *   An array containing the history of issue followups.
 * @param $display_files
 *   Boolean indicating if file attachments should be displayed.
 * @param $recipient
 *   User object indicating recipient's notification preferences.
 * @return
 *   A string of the email body.
 */
function project_issue_mail_generate_followup_mail_body($node, $history, $display_files, $recipient = NULL) {
  global $user;
  static $cache = array();

  $mail_body = $recipient->project_issue_notification['mail_body'];

  // Return cached output if available.
  if (isset($cache[$display_files][$mail_body])) {
    return $cache[$display_files][$mail_body];
  }

  // Get most recent update.
  $entry = array_pop($history);

  $node->project_issue['updator'] = $entry->name ? $entry->name : $user->name;

  // Check if the latest entry is actually the initial issue.
  if (empty($history)) {
    $metadata_previous = new stdClass();
    // Have to get the metadata into the entry object.
    $metadata_entry = $entry->original_issue_metadata;
    $content = $entry->body;
  }
  else {
    $metadata_previous = end($history);
    // If the previous was the original issue, then we need to pull
    // out the metadata from project_issue.
    if (isset($metadata_previous->original_issue_metadata)) {
      $metadata_previous = $metadata_previous->original_issue_metadata;
    }
    $metadata_entry = $entry;
    $content = $entry->comment;
  }

  $fields = project_issue_field_labels('email');
  $comment_changes = project_issue_metadata_changes($node, $metadata_previous, $metadata_entry, $fields);

  // Since $node->name will always be the original issue author, and since
  // $node->project_issue['updator'] isn't a property of either $previous or
  // $entry, these two properties will never show up as being different when
  // project_issue_metadata_changes() is called, and therefore neither of
  // these will ever be elements of the $comment_changes array.  Since we do
  // want them to be printed in issue emails, we just need to add their labels
  // back into the $comment_changes array here, so that
  // theme_project_issue_mail_summary_field() will know to print the data for
  // these two fields.
  $comment_changes['name'] = array(
    'label' => $fields['name'],
  );
  $comment_changes['updator'] = array(
    'label' => $fields['updator'],
  );

  $summary = theme('project_issue_mail_summary', $entry, $node, $comment_changes, $display_files);

  // Create main body content
  project_issue_mail_output($content, 1, $entry->format);
  $body = "$content\n$entry->name\n";

  // Append complete follow-up history if recient prefers that.
  if ($mail_body == PROJECT_ISSUE_MAIL_BODY_FULL_HISTORY) {
    $hr = str_repeat('-', 72);

    if (count($history)) {
      $body .= "\n\n";
      $body .= t('Original issue:') . "\n";
      $body .= project_issue_mail_format_entry(array_shift($history), $display_files, TRUE);
      if (count($history)) {
        $body .= "\n" . t('Previous comments (!count):', array('!count' => count($history))) . "\n";
        foreach ($history as $entry) {
          $body .= project_issue_mail_format_entry($entry, $display_files);
        }
      }
    }
  }
  $output = "$summary\n$body";

  // Set cached output.
  $cache[$display_files][$mail_body] = $output;

  return $output;
}

/**
 * Themes the display of the issue metadata summary
 * that is shown at the top of an issue email.
 *
 * @param $entry
 *  The object representing the current entry.  This will be a node object
 *  if the current entry is the original issue node; otherwise this will be
 *  a comment object.
 * @param $node
 *  The original issue node object.
 * @param $changes
 *  A nested array containing the metadata changes between the original
 *  issue and the first comment, or two consecutive comments.  This array
 *  is the output of the project_issue_metadata_changes() function.
 * @param $display_files
 *   Boolean indicating if file attachments should be displayed.
 * @return
 *   A string containing the themed text of the issue metadata table.
 */
function theme_project_issue_mail_summary($entry, $node, $changes, $display_files) {
  // Mail summary (status values).
  $summary = '';
  foreach ($changes as $field => $change) {
    $summary .= theme('project_issue_mail_summary_field', $node, $field, $change);
  }

  $summary .= project_issue_mail_format_attachments($entry, $display_files);
  return $summary;
}

/**
 * Theme the email output of one project issue metadata field.
 *
 * @param $node
 *   The project issue node object.
 * @param $field_name
 *   The name of the field to theme.
 * @param $change
 *   A nested array containing changes to project issue metadata
 *   for the given issue or comment.
 * @return
 *  A themed line or lines of text ready for inclusion into the email body.
 */
function theme_project_issue_mail_summary_field($node, $field_name, $change) {
  // We need to run the label name through strip_tags here so that
  // the spacing isn't messed up if there are HTML tags in $change['label'].
  $text = str_pad(strip_tags($change['label']) . ':', 14);
  $summary_row = '';
  if (!empty($change['label']) && isset($change['old']) && isset($change['new']) && $field_name != 'updator' && $field_name != 'name') {
    if (is_array($change['old']) || is_array($change['new'])) {
      $removed = array();
      if (is_array($change['old'])) {
        foreach ($change['old'] as $item) {
          $removed[] = '-' . $item;
        }
      }
      elseif (!empty($change['old'])) {
        $removed[] = '-' . $change['old'];
      }

      $added = array();
      if (is_array($change['new'])) {
        foreach ($change['new'] as $item) {
          $added[] = '+' . $item;
        }
      }
      elseif (!empty($change['new'])) {
        $added[] = '+' . $change['new'];
      }

      $summary_row = " $text" . trim(implode(', ', $removed) . '  ' . implode(', ', $added)) . "\n";
    }
    else {
      $summary_row .= "-$text" . project_issue_change_summary($field_name, $change['old']) . "\n";
      $summary_row .= "+$text" . project_issue_change_summary($field_name, $change['new']) . "\n";
    }
  }
  elseif (!empty($change['label'])) {
    if (!empty($change['new'])) {
      // This condition is necessary when building the first email message of an
      // issue, since in this case $change['old'] should not exist.
      if (is_array($change['new'])) {
        $summary_row .= " $text" . implode(', ', $change['new']) . "\n";
      }
      else {
        $summary_row .= " $text" . project_issue_change_summary($field_name, $change['new']) . "\n";
      }
    }
    else {
      // This condition is where fields that are stored in the $node object and
      // which haven't changed but should be printed anyway get processed.
      // For example, the project, category, etc. are printed in each email
      // whether or not they have changed.
      // @TODO: Should we really assume the field in is $node->project_issue[]?
      if (isset($node->project_issue[$field_name])) {
        $summary_row .= " $text" . project_issue_change_summary($field_name, $node->project_issue[$field_name]) . "\n";
      }
    }
  }
  // HTML tags in the email will make it hard to read, so pass
  // this output through strip_tags().
  return strip_tags($summary_row);
}

/**
 * Formats attachments for issue notification e-mails.
 *
 * @param $entry
 *   An issue or followup object containing the file data.
 * @param $display_files
 *   Boolean indicating if file attachments should be displayed.
 * @return
 *   A formatted string of file attachments.
 */
function project_issue_mail_format_attachments($entry, $display_files) {
  $output = '';
  if ($display_files && is_array($entry->files)) {
    foreach ($entry->files as $file) {
      // Comment upload has it's files in an array, so cast to an object
      // for consistency.
      $file = (object) $file;
      $output .= ' ' . str_pad(t('Attachment') . ':', 14) . file_create_url($file->filepath) . ' (' . format_size($file->filesize) . ")\n";
    }
  }
  return $output;
}

/**
 * Format an issue entry for display in an email.
 *
 * @param entry
 *   The entry to the formatted.
 * @param $display_files
 *   Boolean indicating if file attachments should be displayed.
 * @param is_original
 *   Whether this entry is the original issue or a followup. Followup issues
 *   will be automatically numbered.
 * @return
 *   Formatted text for the entry.
 */
function project_issue_mail_format_entry($entry, $display_files, $is_original = FALSE) {
  static $history_count = 1;
  $hr = str_repeat('-', 72);
  $output = "$hr\n";

  // Nodes and comments have different stamp fields.
  $timestamp = isset($entry->created) ? $entry->created : $entry->timestamp;

  if (!$is_original) {
    $output .= "$entry->subject -- ";
  }

  $output .= format_date($timestamp, 'large') . " : $entry->name\n";

  if (!$is_original) {
    $output .= url("node/$entry->nid", array('fragment' => "comment-$entry->cid", 'absolute' => TRUE)) . "\n";
  }

  $output .= project_issue_mail_format_attachments($entry, $display_files);

  // Must distinguish between nodes and comments -- here we do it
  // by looking for a revision ID.
  if (empty($entry->vid)) {
    $content = $entry->comment;
  }
  else {
    $content = $entry->body;
  }

  project_issue_mail_output($content, 1, $entry->format);

  if ($content) {
    $output .= "\n$content";
  }
  return $output;
}
